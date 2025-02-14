#pragma once

#include <string>
#include <iostream>
#include <cstring>
#include <fstream>
#include <random>
#include <fcntl.h>
#include <vector>
#include "rocksdb/db.h"
#include "rocksdb/options.h"
#include "rocksdb/utilities/options_util.h"
#include "rubble_sync_server.h"

using std::string;
using std::vector;
using rocksdb::LoadOptionsFromFile;

const char* ParseCmdPara( char* argv,const char* para) {
   auto p_target = std::strstr(argv,para);
   if (p_target == nullptr) {
      printf("para error argv[%s] should be %s \n",argv,para);
      return nullptr;
   }
   p_target += std::strlen(para);
   return p_target;
}

/**
 * Example usage:
 *
 *    std::random_device rd;
 *    std::mt19937 gen(rd());
 *    zipf_table_distribution<> zipf(300);
 *
 *    for (int i = 0; i < 100; i++)
 *        printf("draw %d %d\n", i, zipf(gen));
 */
template<class IntType = unsigned long, class RealType = double>
class zipf_table_distribution
{
   public:
      typedef IntType result_type;

      static_assert(std::numeric_limits<IntType>::is_integer, "");
      static_assert(!std::numeric_limits<RealType>::is_integer, "");

      /// zipf_table_distribution(N, s)
      /// Zipf distribution for `N` items, in the range `[1,N]` inclusive.
      /// The distribution follows the power-law 1/n^s with exponent `s`.
      /// This uses a table-lookup, and thus provides values more
      /// quickly than zipf_distribution. However, the table can take
      /// up a considerable amount of RAM, and initializing this table
      /// can consume significant time.
      zipf_table_distribution(const IntType n,
                              const RealType q=1.0) :
         _n(init(n,q)),
         _q(q),
         _dist(_pdf.begin(), _pdf.end())
      {}
      void reset() {}

      IntType operator()(std::mt19937_64& rng)
      {
         return _dist(rng);
      }

      /// Returns the parameter the distribution was constructed with.
      RealType s() const { return _q; }
      /// Returns the minimum value potentially generated by the distribution.
      result_type min() const { return 1; }
      /// Returns the maximum value potentially generated by the distribution.
      result_type max() const { return _n; }

   private:
      std::vector<RealType>               _pdf;  ///< Prob. distribution
      IntType                             _n;    ///< Number of elements
      RealType                            _q;    ///< Exponent
      std::discrete_distribution<IntType> _dist; ///< Draw generator

      /** Initialize the probability mass function */
      IntType init(const IntType n, const RealType q)
      {
         _pdf.reserve(n+1);
         _pdf.emplace_back(0.0);
         for (IntType i=1; i<=n; i++)
            _pdf.emplace_back(std::pow((double) i, -q));
         return n;
      }
};

void RunServer(rocksdb::DB* db, const std::string& server_addr) {
  
   RubbleKvServiceImpl service(db);
   grpc::EnableDefaultHealthCheckService(true);
   grpc::reflection::InitProtoReflectionServerBuilderPlugin();
   ServerBuilder builder;
   service.SpawnBGThreads();

   builder.AddListeningPort(server_addr, grpc::InsecureServerCredentials());
   std::cout << "Server listening on " << server_addr << std::endl;
   builder.RegisterService(&service);
   std::unique_ptr<Server> server(builder.BuildAndStart());
   server->Wait();
   service.FinishBGThreads();
}

void NewLogger(const std::string& log_fname, rocksdb::Env* env, std::shared_ptr<rocksdb::Logger>& logger){
   rocksdb::Status s = rocksdb::NewEnvLogger(log_fname, env, &logger);
   if (logger.get() != nullptr) {
      logger->SetInfoLogLevel(rocksdb::InfoLogLevel::DEBUG_LEVEL);
   }
   if(!s.ok()){
      std::cout << "Error creating log : " << s.ToString() << std::endl;
      assert(false);
      logger = nullptr;
   }
}

/**
 * @param db_path the db path
 * @param sst_dir local sst directory
 * @param remote_sst_dir not "" for non-tail nodes
 * @param target_addr the downstream node's address in the chain, for the non-tail nodes, it's forwarding ops to this address, 
 *                    for the tail node, this target_addr is replicator's address to which it's sending the true reply.
 * @param is_rubble when enabled, compaction is disabled when is_primary sets to false,
 *                   when disabled, all nodes are running flush/compaction
 * @param is_primary set to true for first/primary node in the chain 
 * @param is_tail    set to true for tail node in the chain
 * @param shard_id   used as the unique identifier of logs
 */
rocksdb::DB* GetDBInstance(const string& db_path, const string& sst_dir, 
                                const string& remote_sst_dir,
                                const string& sst_pool_dir,
                                const string& target_addr, 
                                const string& primary_addr,
                                bool is_rubble, bool is_primary, bool is_tail,
                                const string& shard_id, const vector<string>& remote_sst_dirs = vector<string>(),
                                int rid = -1, int rf = 3) {

   rocksdb::DB* db;
   rocksdb::DBOptions db_options;
   rocksdb::ConfigOptions config_options;
   std::vector<rocksdb::ColumnFamilyDescriptor> loaded_cf_descs;
   rocksdb::Status s = LoadOptionsFromFile(
      /*config_options=*/config_options,
      /*options_file_name=*/is_primary ? "/mnt/data/rocksdb/rubble/rubble_16gb_config.ini" : "/mnt/data/rocksdb/rubble/rubble_16gb_config_tail.ini",
      /*db_options=*/&db_options,
      /*cf_descs=*/&loaded_cf_descs
   );
   if (!s.ok()) std::cout << s.getState() << '\n';
   assert(s.ok());

   std::cout << "number of cf: " << loaded_cf_descs.size() << '\n';
   assert(loaded_cf_descs.size() == 1); // We currently only support one ColumnFamily
   
   // Optimize RocksDB. This is the easiest way to get RocksDB to perform well
   // db_options.IncreaseParallelism();
   // db_options.is_rubble=is_rubble;
   db_options.is_primary=is_primary;
   db_options.is_tail=is_tail;

   std::cout << "is_rubble: " << db_options.is_rubble << '\n';
   std::cout << "is_primary: " << db_options.is_primary << '\n';
   std::cout << "is_tail: " << db_options.is_tail << '\n';
   
   db_options.target_address=target_addr; //TODO(add target_addr, remote_sst_dir and preallocated_sst_pool_size to option file)
   db_options.primary_address = primary_addr;

   // for non-tail nodes in rubble mode, it's shipping sst file to the remote_sst_dir;
   if (db_options.is_rubble && !is_tail) {
       db_options.remote_sst_dir = remote_sst_dir;
       db_options.remote_sst_dirs = remote_sst_dirs;
       std::cout << "remote sst dir: " << db_options.remote_sst_dir << std::endl;
       for (string dir : db_options.remote_sst_dirs) {
            std::cout << "remote sst dirs: " << dir << std::endl;
       }
   }

   if (db_options.is_rubble && !is_primary) {
       db_options.sst_pool_dir = sst_pool_dir;
       std::cout << "sst pool dir: " << db_options.sst_pool_dir << std::endl;
   }
   
   uint64_t target_size = 10000000000;
   db_options.db_paths.emplace_back(rocksdb::DbPath(sst_dir, 10000000000));
   
   rocksdb::ColumnFamilyOptions cf_options = loaded_cf_descs[0].options;

   db_options.env = rocksdb::Env::Default();
   db_options.rf = rf;
   db_options.rid = rid;

   // add logger for rubble
   // the default path for the sst bit map log file, will try to reconstruct map from this file
   std::string rubble_log_path = db_path + "/"; 
   std::string rubble_info_log_fname;
   std::string map_log_fname;

   std::shared_ptr<rocksdb::Logger> logger = nullptr;
   std::shared_ptr<rocksdb::Logger> map_logger = nullptr;

   db_options.env->CreateDirIfMissing(rubble_log_path).PermitUncheckedError(); 
   if(db_options.is_primary) {
      rubble_info_log_fname = rubble_log_path + shard_id + "_primary_log";
      map_log_fname = rubble_log_path + shard_id + "_primary_sst_map_log";
   } else if (db_options.is_tail) {
      rubble_info_log_fname = rubble_log_path + shard_id + "_tail_log";
      map_log_fname = rubble_log_path + shard_id + "_tail_sst_map_log";
   } else {
      rubble_info_log_fname = rubble_log_path + shard_id + "_secondary_log";
      map_log_fname = rubble_log_path + shard_id + "_secondary_sst_map_log";
   }

   std::string current_fname = rocksdb::CurrentFileName(db_path);

   std::cout<<"current_fname: "<<current_fname<<std::endl<<std::flush;
   // It's not a clean start
   s = db_options.env->FileExists(current_fname);
   assert(!s.ok());
   s = db_options.env->FileExists(map_log_fname);
   assert(!s.ok());

  std::cout << "[info log fname] " << rubble_info_log_fname << "\n";
  std::cout << "[sst map log fname] " << map_log_fname << "\n";

  // create rubble info logger
   NewLogger(rubble_info_log_fname, db_options.env, logger);
   db_options.rubble_info_log = logger;

   NewLogger(map_log_fname, db_options.env, map_logger);

   if(db_options.target_address != "") {
      db_options.channel = grpc::CreateChannel(db_options.target_address, grpc::InsecureChannelCredentials());
   }

   if (db_options.primary_address != "") {
      db_options.primary_channel = grpc::CreateChannel(db_options.primary_address, grpc::InsecureChannelCredentials());
   }

   if(db_options.is_rubble) {
      //ignore this flag for now, always set to true.
      db_options.disallow_flush_on_secondary = true;

      db_options.max_num_mems_in_flush = 1;
      db_options.sst_pad_len = 1 << 20;
      db_options.piggyback_version_edits = false;
      db_options.edits = std::make_shared<Edits>();

      // if(!db_options.is_tail && !db_options.piggyback_version_edits) {
      //    assert(db_options.target_address != "");
      //    db_options.sync_client = std::make_shared<SyncClient>(db_options.channel);
      // }
      // a default LSM tree has up to 4448 SST files (4 L0, 4 L1, 40 L2, 400 L3, 4000 L4 files)
      // but we target 16GB per instance, so only need 1000 files at L4
      db_options.preallocated_sst_pool_size = 1448;
      // db_options.preallocated_sst_pool_size = db_options.db_paths.front().target_size / (((cf_options.write_buffer_size >> 20) + 1) << 20);
      db_options.sst_bit_map = std::make_shared<SstBitMap>(
            db_options.preallocated_sst_pool_size, 
            db_options.max_num_mems_in_flush,
            db_options.is_primary,
            db_options.rf,
            db_options.rubble_info_log,
            map_logger);
   }

   // if(!db_options.is_primary){
   //    db_options.use_direct_reads = true;
   // }else{
   //    db_options.use_direct_reads = false;
   // }
   db_options.use_direct_reads = true;
   db_options.use_direct_io_for_flush_and_compaction = true;

   std::cout << "write_buffer_size: " << cf_options.write_buffer_size << '\n';
   std::cout << "target_file_size_base: " << cf_options.target_file_size_base << '\n';
   rocksdb::Options options(db_options, cf_options);

   
   // options.table_factory.reset(rocksdb::NewBlockBasedTableFactory(table_options));

   options.statistics = rocksdb::CreateDBStatistics();
   // options.statistics->getTickerCount(rocksdb::NUMBER_BLOCK_COMPRESSED);
   // rocksdb::HistogramData hist;
   // options.statistics->histogramData(rocksdb::FLUSH_TIME, &hist);
   // if(db_options.is_rubble && !db_options.is_primary){
   //    // make sure compaction is disabled on the secondary nodes in rubble mode
   //    options.compaction_style = rocksdb::kCompactionStyleNone;
   // }

   // open DB
   s = rocksdb::DB::Open(options, db_path, &db);
   if(!s.ok()){
      std::cout << "DB open failed : " << s.ToString() << std::endl;
   }
   assert(s.ok());
   return db;
}

