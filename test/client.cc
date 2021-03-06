#include "client.h"

#include <unistd.h>
#include <getopt.h>
#include <algorithm>
#include "logger.h"
#include "client.pb.h"

namespace client {

void Tokenize(const std::string& str, std::vector<std::string>& tokens, const char& delimiter = ' ') {
  size_t prev_pos = str.find_first_not_of(delimiter, 0);
  size_t pos = str.find(delimiter, prev_pos);

  while (prev_pos != std::string::npos || pos != std::string::npos) {
    std::string token(str.substr(prev_pos, pos - prev_pos));
    //printf ("find a token(%s), prev_pos=%u pos=%u\n", token.c_str(), prev_pos, pos);
    tokens.push_back(token);

    prev_pos = str.find_first_not_of(delimiter, pos);
    pos = str.find_first_of(delimiter, prev_pos);
  }
}

///// Server //////
Server::Server(const std::string& str) {
  size_t pos = str.find(':');
  ip = str.substr(0, pos);
  port = atoi(str.substr(pos + 1).c_str());
}

///// Option //////
Option::Option()
    : timeout(1000) {
    }

Option::Option(const std::string& server_str) 
  : timeout(1000) {
  std::vector<std::string> server_list;
  Tokenize(server_str, server_list, ',');
  Init(server_list);
}

Option::Option(const std::vector<std::string>& server_list)
  : timeout(1000) {
  Init(server_list); 
}

Option::Option(const Option& option)
  : timeout(option.timeout) {
    std::copy(option.servers.begin(), option.servers.end(), std::back_inserter(servers));
  }


void Option::Init(const std::vector<std::string>& server_list) {
  for (auto it = server_list.begin(); it != server_list.end(); it++) {
    servers.push_back(Server(*it));
  }
}

void Option::ParseFromArgs(int argc, char *argv[]) {
  if (argc < 2) {
    LOG_ERROR("invalid arguments!");
    abort();
  }

  static struct option const long_options[] = {
    {"server", required_argument, NULL, 's'},
    {NULL, 0, NULL, 0} };

  std::string server_str;
  int opt, optindex;
  while ((opt = getopt_long(argc, argv, "s:", long_options, &optindex)) != -1) {
    switch (opt) {
      case 's':
        server_str = optarg;
        break;
      default:
        break;
    }
  }

  std::vector<std::string> server_list;

  Tokenize(server_str, server_list, ',');
  Init(server_list);
}

////// Cluster //////
Cluster::Cluster(const Option& option)
  : option_(option),
  meta_cli_(new pink::PbCli),
  pb_cli_(new pink::PbCli) {
  Init();
}

void Cluster::Init() {
  if (option_.servers.size() < 1) {
    LOG_ERROR("cluster has no server!");
    abort();
  }
  // TEST use the first server
 // pink::Status result = pb_cli_->Connect("127.0.0.1", 8001);
 // LOG_INFO("connect DataServer(127.0.0.1:8001), %s", result.ToString().c_str());
  //pink::Status result = pb_cli_->Connect(option_.servers[0].ip, option_.servers[0].port);
  //LOG_INFO("connect DataServer(%s:%d), %s", option_.servers[0].ip.c_str(), option_.servers[0].port, result.ToString().c_str());
//  if (!result.ok()) {
//    LOG_ERROR("cluster connect(%s:%d) error, %s", option_.servers[0].ip, option_.servers[0].port, result.ToString().c_str());
//  }

  //result = meta_cli_->Connect("127.0.0.1", 8101);
  //LOG_INFO("connect DataServer's MetacmdWorker(127.0.0.1:8101), %s", result.ToString().c_str());
  //LOG_INFO("connect DataServer's MetacmdWorker(%s:%d), %s", option_.servers[0].ip.c_str(), option_.servers[0].port + 100, result.ToString().c_str());

  hb_thread_ = new ZPHeartbeatThread(option_.servers[0].port + 100, 2000);
  hb_thread_->StartThread();
}

Status Cluster::Set(const std::string& key, const std::string& value, std::string ip, int port) {
  pink::Status result = pb_cli_->Connect(ip, port);
  LOG_INFO("connect DataServer(%s:%d), %s", ip.c_str(), port, result.ToString().c_str());

  CmdRequest request;
  request.set_type(Type::SET);

  CmdRequest_Set* set_req = request.mutable_set();
  set_req->set_key(key);
  set_req->set_value(value);


  result = pb_cli_->Send(&request);
  if (!result.ok()) {
    LOG_ERROR("Send error: %s", result.ToString().c_str());
    return Status::IOError("Send failed, " + result.ToString());
  }

  CmdResponse response;
  result = pb_cli_->Recv(&response);
  if (!result.ok()) {
    LOG_ERROR("Recv error: %s", result.ToString().c_str());
    return Status::IOError("Recv failed, " + result.ToString());
  }

  pb_cli_->Close();
  if (response.set().code() == StatusCode::kOk) {
    LOG_INFO("Set OK, status is %d, msg is %s\n", response.set().code(), response.set().msg().c_str());
    return Status::OK();
  } else if (response.set().code() == StatusCode::kNotFound) {
    LOG_INFO("Set Not found, status is %d, msg is %s\n", response.set().code(), response.set().msg().c_str());
    return Status::NotFound("");
  } else {
    return Status::Corruption(response.set().msg());
  }
}

Status Cluster::Update(ZPMeta::MetaCmd &request, ZPMeta::MetaCmdResponse &response, std::string ip, int port) {
  pink::Status result = meta_cli_->Connect(ip, port);
  LOG_INFO("connect DataServer's MetacmdWorker(%s:%d), %s", ip.c_str(), port, result.ToString().c_str());
  result = meta_cli_->Send(&request);
  if (!result.ok()) {
    LOG_ERROR("Update Send error: %s", result.ToString().c_str());
    return Status::IOError("Update Send failed, " + result.ToString());
  }

  result = meta_cli_->Recv(&response);
  if (!result.ok()) {
    LOG_ERROR("Update Recv error: %s", result.ToString().c_str());
    return Status::IOError("Update Recv failed, " + result.ToString());
  }

  meta_cli_->Close();
  return Status::OK();
}

Status Cluster::Get(const std::string& key, std::string* value, std::string ip, int port) {
  pink::Status result = pb_cli_->Connect(ip, port);
  LOG_INFO("connect DataServer(%s:%d), %s", ip.c_str(), port, result.ToString().c_str());

  CmdRequest request;
  request.set_type(Type::GET);

  CmdRequest_Get* get_req = request.mutable_get();
  get_req->set_key(key);

  result = pb_cli_->Send(&request);
  if (!result.ok()) {
    LOG_ERROR("Send error: %s", result.ToString().c_str());
    return Status::IOError("Send failed, " + result.ToString());
  }

  CmdResponse response;
  result = pb_cli_->Recv(&response);
  if (!result.ok()) {
    LOG_ERROR("Recv error: %s", result.ToString().c_str());
    return Status::IOError("Recv failed, " + result.ToString());
  }

  *value = response.get().value();

  pb_cli_->Close();
  LOG_INFO("Get OK, status is %d, value is %s\n", response.get().code(), response.get().value().c_str());
  return Status::OK();
}

////// ZPPbCli //////
void ZPPbCli::BuildWbuf() {
  uint32_t len;
  wbuf_len_ = msg_->ByteSize();
  len = htonl(wbuf_len_ + 4);
  memcpy(wbuf_, &len, sizeof(uint32_t));
  len = htonl(opcode_);
  memcpy(wbuf_ + 4, &len, sizeof(uint32_t));
  msg_->SerializeToArray(wbuf_ + 8, wbuf_len_);
  wbuf_len_ += 8;

  //printf ("wbuf_[0-4]  bytesize=%d len=%d\n", wbuf_len_, len);
}

} // namespace client
