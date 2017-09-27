// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.

#include "yb/master/call_home.h"

#include <rapidjson/reader.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <sstream>
#include <thread>

#include <boost/algorithm/string.hpp>
#include <boost/optional.hpp>
#include <gflags/gflags.h>

#include "yb/master/catalog_manager.h"
#include "yb/master/master.pb.h"

static const char* kLowLevel = "low";
static const char* kMediumLevel = "medium";
static const char* kHighLevel = "high";

DEFINE_bool(callhome_enabled, true,
            "Enables callhome feature that sends analytics data to yugabyte");
DEFINE_int32(callhome_interval_secs, 60, "How often to run callhome");
DEFINE_string(callhome_url, "http://diagnostics.platform.yugabyte.com:8080",
              "URL of callhome server");
DEFINE_string(callhome_collection_level, kMediumLevel, "Level of details sent by callhome");

using google::CommandlineFlagsIntoString;
using strings::Substitute;
using yb::master::Master;
using yb::master::ListMastersResponsePB;
using yb::master::ListTablesRequestPB;
using yb::master::ListTablesResponsePB;
using yb::master::ListTabletServersResponsePB;
using yb::master::GetMasterClusterConfigResponsePB;
using yb::master::TSDescriptor;
using yb::server::RpcAndWebServerBase;
using yb::tserver::TabletServer;

namespace yb {

Collector::~Collector() {}

class CollectorBase : public Collector {
 public:
  CollectorBase(server::RpcAndWebServerBase* server, ServerType server_type);

  virtual ~CollectorBase();

  bool Run(CollectionLevel collection_level);
  virtual void Collect(CollectionLevel collection_level) = 0;

  const std::string& as_json() { return json_; }
  ServerType server_type() { return server_type_; }

  virtual std::string collector_name() = 0;

  virtual CollectionLevel collection_level() = 0;
  virtual ServerType collector_type() = 0;

 protected:
  inline master::Master* master() { return down_cast<master::Master*>(server_); }
  inline tserver::TabletServer* tserver() { return down_cast<tserver::TabletServer*>(server_); }

  server::RpcAndWebServerBase* server_;
  ServerType server_type_;
  std::string json_;
};

CollectorBase::~CollectorBase() {}

CollectorBase::CollectorBase(RpcAndWebServerBase* server, ServerType server_type):
    server_(server), server_type_(server_type) {}

bool CollectorBase::Run(CollectionLevel level) {
  json_.clear();
  if (collector_type() == ServerType::ALL || collector_type() == server_type_) {
    if (collection_level() <= level) {
      Collect(level);
      return true;
    } else {
      LOG(INFO) << "Skipping collector " << collector_name()
                << " because it has a higher collection level than the requested one";
    }
  } else {
    LOG(INFO) << "Skipping collector " << collector_name() << " because of server type";
  }
  return false;
}

namespace {

void AppendPairToJson(const string &key, const string &value, string *out) {
  if (!out->empty()) {
    *out += ",";
  }
  *out += Substitute("\"$0\":\"$1\"", key, value);
}
} // namespace

class BasicCollector : public CollectorBase {
 public:
  using CollectorBase::CollectorBase;
  void Collect(CollectionLevel collection_level) {
    switch (server_type_) {
      case ServerType::ALL:
        LOG(FATAL) << "Invalid server type ALL";
      case ServerType::MASTER: {
        master::SysClusterConfigEntryPB config;
        auto status = master()->catalog_manager()->GetClusterConfig(&config);
        if (status.ok()) {
          AppendPairToJson("cluster_uuid", config.cluster_uuid(), &json_);
        }
        AppendPairToJson("node_uuid", master()->fs_manager()->uuid(), &json_);
        AppendPairToJson("server_type", "master", &json_);
        break;
      }
      case ServerType::TSERVER: {
        AppendPairToJson("cluster_uuid", tserver()->cluster_uuid(), &json_);
        AppendPairToJson("node_uuid", tserver()->permanent_uuid(), &json_);
        AppendPairToJson("server_type", "tserver", &json_);
        break;
      }
    }
    AppendPairToJson("timestamp", std::to_string(WallTime_Now()), &json_);
  }

  string collector_name() { return "BasicCollector"; }

  virtual CollectionLevel collection_level() { return CollectionLevel::LOW; }
  virtual ServerType collector_type() { return ServerType::ALL; }
};

class MetricsCollector : public CollectorBase {
 public:
  using CollectorBase::CollectorBase;

  void Collect(CollectionLevel collection_level) {
    std::stringstream s;
    JsonWriter w(&s, JsonWriter::COMPACT);

    Status status = server_->metric_registry()->WriteAsJson(&w, {"*"}, MetricJsonOptions());
    if (!status.ok()) {
      json_ = "\"metrics\":{}";
      return;
    }
    json_ = "\"metrics\":" + s.str();
  }

  string collector_name() { return "MetricsCollector"; }

  virtual CollectionLevel collection_level() { return CollectionLevel::MEDIUM; }
  virtual ServerType collector_type() { return ServerType::ALL; }
};

class RpcsCollector : public CollectorBase {
 public:
  using CollectorBase::CollectorBase;

  void Collect(CollectionLevel collection_level) {
    if (!UpdateAddr().ok()) {
      json_ = "\"rpcs\":{}";
      return;
    }

    faststring buf;
    auto url = Substitute("http://$0/rpcz", yb::ToString(*addr_));
    auto status = curl_.FetchURL(url, &buf);
    if (!status.ok()) {
      LOG(ERROR) << "Unable to read url " << url;
      return;
    }

    if (buf.length() > 0) {
      auto rpcs_json = buf.ToString();
      boost::replace_all(rpcs_json, "\n", "");
      json_ = "\"rpcs\":" +  rpcs_json;
    } else {
      LOG(WARNING) << "Error getting rpcs";
    }

  }

  string collector_name() { return "RpcsCollector"; }

  virtual CollectionLevel collection_level() { return CollectionLevel::MEDIUM; }
  virtual ServerType collector_type() { return ServerType::ALL; }

 private:
  CHECKED_STATUS UpdateAddr() {
    if (addr_) {
      return Status::OK();
    }

    vector<Endpoint> addrs;
    auto status = server_->web_server()->GetBoundAddresses(&addrs);
    if (!status.ok()) {
      LOG(WARNING) << "Unable to get webserver address: " << status.ToString();
      return STATUS(InternalError, "Unable to get webserver address");
    }
    addr_.emplace(addrs[0]);
    return Status::OK();
  }

  boost::optional<Endpoint> addr_;
  EasyCurl curl_;
};

class TablesCollector : public CollectorBase {
 public:
  using CollectorBase::CollectorBase;

  void Collect(CollectionLevel collection_level) {
    ListTablesRequestPB req;
    ListTablesResponsePB resp;
    auto status = master()->catalog_manager()->ListTables(&req, &resp);
    if (collection_level == CollectionLevel::LOW) {
      json_ = Substitute("\"tables\":$0", resp.tables_size());
    } else {
      // TODO(hector): Add more table details.
      json_ = Substitute("\"tables\":$0", resp.tables_size());
    }
  }

  string collector_name() { return "TablesCollector"; }

  virtual CollectionLevel collection_level() { return CollectionLevel::ALL; }
  virtual ServerType collector_type() { return ServerType::MASTER; }
};

class MasterInfoCollector : public CollectorBase {
 public:
  using CollectorBase::CollectorBase;

  void Collect(CollectionLevel collection_level) {
    vector<ServerEntryPB> masters;
    Status s = master()->ListMasters(&masters);
    if (s.ok()) {
      if (collection_level == CollectionLevel::LOW) {
        json_ = Substitute("\"masters\":$0", masters.size());
      } else {
        // TODO(hector): Add more details.
        json_ = Substitute("\"masters\":$0", masters.size());
      }
    }
  }

  string collector_name() { return "MasterInfoCollector"; }

  virtual CollectionLevel collection_level() { return CollectionLevel::ALL; }
  virtual ServerType collector_type() { return ServerType::MASTER; }
};

class TServersInfoCollector : public CollectorBase {
 public:
  using CollectorBase::CollectorBase;

  void Collect(CollectionLevel collection_level) {
    vector<std::shared_ptr<TSDescriptor>> descs;
    master()->ts_manager()->GetAllDescriptors(&descs);
    if (collection_level == CollectionLevel::LOW) {
      json_ = Substitute("\"tservers\":$0", descs.size());
    } else {
      // TODO(hector): Add more details.
      json_ = Substitute("\"tservers\":$0", descs.size());
    }
  }

  string collector_name() { return "TServersInfoCollector"; }

  virtual CollectionLevel collection_level() { return CollectionLevel::ALL; }
  virtual ServerType collector_type() { return ServerType::MASTER; }
};

class TabletsCollector : public CollectorBase {
 public:
  using CollectorBase::CollectorBase;

  void Collect(CollectionLevel collection_level) {
    int ntablets;
    if (server_type_ == ServerType::MASTER) {
      ntablets = 1;
    } else {
      ntablets = tserver()->tablet_manager()->GetNumLiveTablets();
    }
    json_ = Substitute("\"tablets\":$0", ntablets);
  }

  string collector_name() { return "TabletsCollector"; }

  virtual CollectionLevel collection_level() { return CollectionLevel::ALL; }
  virtual ServerType collector_type() { return ServerType::ALL; }
};

class GFlagsCollector : public CollectorBase {
 public:
  using CollectorBase::CollectorBase;

  void Collect(CollectionLevel collection_level) {
    auto gflags = CommandlineFlagsIntoString();
    boost::replace_all(gflags, "\n", " ");
    json_ = Substitute("\"gflags\":\"$0\"", gflags);
  }

  string collector_name() { return "GFlagsCollector"; }

  virtual CollectionLevel collection_level() { return CollectionLevel::LOW; }
  virtual ServerType collector_type() { return ServerType::ALL; }
};

CallHome::CallHome(server::RpcAndWebServerBase* server, ServerType server_type) :
    server_(server), pool_(1), server_type_(server_type) {

  scheduler_ = std::make_unique<yb::rpc::Scheduler>(&pool_.io_service());

  AddCollector<BasicCollector>();
  AddCollector<MasterInfoCollector>();
  AddCollector<TServersInfoCollector>();
  AddCollector<TablesCollector>();
  AddCollector<TabletsCollector>();
  AddCollector<MetricsCollector>();
  AddCollector<RpcsCollector>();
  AddCollector<GFlagsCollector>();
}

CallHome::~CallHome() {
  scheduler_->Shutdown();
  pool_.Shutdown();
  pool_.Join();
}

template <typename T>
void CallHome::AddCollector() {
  collectors_.emplace_back(std::make_unique<T>(server_, server_type_));
}

std::string CallHome::BuildJson() {
  string str = "{";
  string comma = "";
  auto collection_level = GetCollectionLevel();
  for (const auto& collector : collectors_) {
    if (collector->Run(collection_level) && !collector->as_json().empty()) {
      str += comma;
      str += collector->as_json();
      comma = ",";
      LOG(INFO) << "Done with collector " << collector->collector_name();
    }
  }
  str += "}";

  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  rapidjson::Reader reader;
  rapidjson::StringStream ss(str.c_str());
  if (!reader.Parse<rapidjson::kParseDefaultFlags>(ss, writer)) {
    LOG(ERROR) << "Unable to parse json. Error: " << reader.GetParseError()
        << " at offset " << reader.GetErrorOffset() << " in string " <<
        str.substr(reader.GetErrorOffset(), 10);
    return str;
  }

  return buffer.GetString();
}

void CallHome::BuildJsonAndSend() {
  auto json = BuildJson();
  SendData(json);
}

void CallHome::DoCallHome() {
  ScheduleCallHome();

  if (server_type_ == ServerType::MASTER &&
      !master()->catalog_manager()->CheckIsLeaderAndReady().ok()) {
    LOG(INFO) << "This master instance is not a leader. Skipping call home";
    return;
  }

  BuildJsonAndSend();
}

void CallHome::SendData(const string& payload) {
  faststring reply;

  auto status = curl_.PostToURL(FLAGS_callhome_url, payload, "application/json", &reply);
  if (!status.ok()) {
    LOG(ERROR) << "Error sending data to " << FLAGS_callhome_url;
  }
  VLOG(1) << "Received reply: " << reply;
}

void CallHome::ScheduleCallHome() {
  scheduler_->Schedule(std::bind(&CallHome::DoCallHome, this),
                       std::chrono::seconds(FLAGS_callhome_interval_secs));
}

CollectionLevel CallHome::GetCollectionLevel() {
  if (FLAGS_callhome_collection_level == kHighLevel) {
    return CollectionLevel::HIGH;
  } else if (FLAGS_callhome_collection_level == kMediumLevel) {
    return CollectionLevel::MEDIUM;
  } else if (FLAGS_callhome_collection_level == kLowLevel) {
    return CollectionLevel::LOW;
  }
  return CollectionLevel::LOW;
}
} // namespace yb
