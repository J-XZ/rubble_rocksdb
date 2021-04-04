#include "util.h"
#include "rubble_server.h"

/* tail node in the chain */
int main(int argc, char** argv) {

  if (argc != 2) {
      std::cout << "Usage:./program replicator's server address\n";
      return 0;
  }
  //int thread_num = std::atoi(ParseCmdPara(argv[1],"--thread="));
  //std::string target_addr = std::atoi(ParseCmdPara(argv[2],"--target"));
  //server is running on localhost:50049;
  const std::string target_addr = argv[1];
  const std::string server_addr = "128.110.154.78:50052"; 
  rocksdb::DB* db = GetDBInstance("/tmp/rubble_tail", "/mnt/sdb/archive_dbs/tail/sst_dir","", target_addr, false, false, true);

  RunServer(db, server_addr, 16);
  return 0;
}
