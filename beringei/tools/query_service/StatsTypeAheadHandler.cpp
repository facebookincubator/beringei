/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "mysql_connection.h"
#include "mysql_driver.h"
#include "StatsTypeAheadHandler.h"

#include <algorithm>
#include <cppconn/driver.h>
#include <cppconn/exception.h>
#include <cppconn/prepared_statement.h>
#include <cppconn/resultset.h>
#include <cppconn/statement.h>
#include <folly/DynamicConverter.h>
#include <folly/Conv.h>
#include <folly/io/IOBuf.h>
#include <map>
#include <proxygen/httpserver/ResponseBuilder.h>
#include <thrift/lib/cpp/util/ThriftSerializer.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>
#include <utility>

using apache::thrift::SimpleJSONSerializer;
using std::chrono::duration_cast;
using std::chrono::milliseconds;
using std::chrono::system_clock;
using namespace proxygen;

namespace facebook {
namespace gorilla {

StatsTypeAheadHandler::StatsTypeAheadHandler(
    std::shared_ptr<MySqlClient> mySqlClient)
    : RequestHandler(),
      mySqlClient_(mySqlClient) {
  metricKeyNames_ = { "snr", "rssi", "mcs", "per", "fw_uptime", "tx_power",
                      "rx_bytes", "tx_bytes", "rx_pps", "tx_pps", "rx_errors",
                      "tx_errors", "rx_dropped", "tx_dropped", "rx_frame",
                      "rx_overruns", "tx_overruns", "tx_collisions", "speed" };
}

void StatsTypeAheadHandler::onRequest(
    std::unique_ptr<HTTPMessage> /* unused */) noexcept {
  // nothing to do
}

void StatsTypeAheadHandler::onBody(std::unique_ptr<folly::IOBuf> body) noexcept {
  if (body_) {
    body_->prependChain(move(body));
  } else {
    body_ = move(body);
  }
}

void StatsTypeAheadHandler::fetchMetricNames(query::Topology& request) {
  folly::dynamic nodeData;
  nodeMetrics_ = folly::dynamic::object;
  siteMetrics_ = folly::dynamic::object;

  if (request.nodes.empty()) {
    LOG(ERROR) << "No nodes in topology, failing request";
    return;
  }
      
  for (auto& node : request.nodes) {
    macNodes_.insert(node.mac_addr);
    nodesByName_[node.name] = node;
  }

  auto dbNodeKeys = mySqlClient_->getNodesWithKeys(macNodes_);
  for (auto& node : dbNodeKeys) {
    folly::StringPiece key = folly::StringPiece(node.key);
    if (key.endsWith("count.0") ||
        key.endsWith("count.600") ||
        key.endsWith("count.3600")) {
      continue;
    }
    if (nodeMetrics_.find(node.mac) == nodeMetrics_.items().end()) {
      nodeMetrics_[node.mac] = folly::dynamic::object;
    }
    nodeMetrics_[node.mac][node.key] = folly::dynamic::object("dbKeyId", node.id);
  }
  
  for (auto& node : request.nodes) {
    if (nodeMetrics_.find(node.mac_addr) != nodeMetrics_.items().end()) {
      nodeData = nodeMetrics_[node.mac_addr];
      if (siteMetrics_.find(node.site_name) == siteMetrics_.items().end()) {
        siteMetrics_[node.site_name] = folly::dynamic::object;
      }
      if (siteMetrics_[node.site_name].find(node.name) == siteMetrics_[node.site_name].items().end()) {
        siteMetrics_[node.site_name][node.name] = folly::dynamic::array;
      }
      siteMetrics_[node.site_name][node.name].push_back(nodeData);
    }
  }
    
  for(auto& link : request.links) {
    auto aNode = nodesByName_[link.a_node_name];
    auto zNode = nodesByName_[link.z_node_name];
    for (auto& metricName : metricKeyNames_) {
      folly::dynamic linkMetrics = getLinkMetrics(metricName, aNode, zNode);
      for (auto& key : linkMetrics["keys"]) {
      	auto node = SimpleJSONSerializer::deserialize<query::Node>(key["node"].asString());
        auto mac = node.mac_addr;
        auto keyName = key["keyName"].asString();
        if (nodeMetrics_.find(mac) == nodeMetrics_.items().end() ||
            nodeMetrics_[mac].find(keyName) == nodeMetrics_[mac].items().end()) {
          continue;
        }
        nodeData = nodeMetrics_[mac];
        nodeData[keyName]["displayName"] = linkMetrics["title"].asString();
        nodeData[keyName]["linkName"] = link.name;
        nodeData[keyName]["linkTitleAppend"] = key["titleAppend"].asString();
        nodeData[keyName]["title"] = linkMetrics["title"].asString() + key["titleAppend"].asString();
        nodeData[keyName]["description"] = linkMetrics["description"].asString();
        nodeData[keyName]["scale"] = linkMetrics["scale"].asString();
        siteMetrics_[node.site_name][node.name].push_back(nodeData);
      } 
    } 
  }
}

folly::dynamic StatsTypeAheadHandler::createLinkMetric(query::Node& aNode, query::Node& zNode,
                                                       std::string title, std::string description,
                                                       std::string keyName, std::string keyPrefix) {
  return folly::dynamic::object
    ("title", title)
    ("description", description)
    ("scale", NULL)
    ("keys", folly::dynamic::array(
      folly::dynamic::object
        ("node", SimpleJSONSerializer::serialize<std::string>(aNode))
        ("keyName", keyPrefix + "." + zNode.mac_addr + "." + keyName)
        ("titleAppend", " (A)"),
      folly::dynamic::object
        ("node", SimpleJSONSerializer::serialize<std::string>(zNode))
        ("keyName", keyPrefix + "." + aNode.mac_addr + "." + keyName)
        ("titleAppend", " (Z)")));
}

folly::dynamic StatsTypeAheadHandler::getLinkMetrics(std::string& metricName, query::Node& aNode, query::Node& zNode) {
  if (metricName == "rssi") {
    return createLinkMetric(aNode, zNode,
                            "RSSI", "Received Signal Strength Indicator",
                            "phystatus.srssi");
  }
  else if (metricName == "snr" || 
           metricName == "alive_snr" || 
           metricName == "alive_perc") {
    return createLinkMetric(aNode, zNode,
                            "SnR", "Signal to Noise Ratio",
                            "phystatus.ssnrEst");
  }
  else if (metricName == "mcs") {
    return createLinkMetric(aNode, zNode,
                            "MCS", "MCS Index",
                            "staPkt.mcs");
  }
  else if (metricName == "per") {
    return createLinkMetric(aNode, zNode,
                            "PER", "Packet Error Rate",
                            "staPkt.perE6");
  }
  else if (metricName == "fw_uptime") {
    return createLinkMetric(aNode, zNode,
                            "FW Uptime", "Mgmt Tx Keepalive Count",
                            "mgmtTx.keepAlive");
  }
  else if (metricName == "rx_ok") {
    return createLinkMetric(aNode, zNode,
                            "RX Packets", "Received packets",
                            "staPkt.rxOk");
  }
  else if (metricName == "tx_ok") {
    return createLinkMetric(aNode, zNode,
                            "TX Packets", "Transferred packets",
                            "staPkt.txOk");
  }
  else if (metricName == "tx_bytes") {
    return createLinkMetric(aNode, zNode,
                            "TX bps", "Transferred bits/second",
                            "tx_bytes", "link");
  }
  else if (metricName == "rx_bytes") {
    return createLinkMetric(aNode, zNode,
                            "RX bps", "Received bits/second",
                            "rx_bytes", "link");
  }
  else if (metricName == "tx_errors") {
    return createLinkMetric(aNode, zNode,
                            "TX errors", "Transmit errors/second",
                            "tx_errors", "link");
  }
  else if (metricName == "rx_errors") {
    return createLinkMetric(aNode, zNode,
                            "RX errors", "Receive errors/second",
                            "rx_errors", "link");
  }
  else if (metricName == "tx_dropped") {
    return createLinkMetric(aNode, zNode,
                            "TX dropped", "Transmit packets/second",
                            "tx_dropped", "link");
  }
  else if (metricName == "rx_dropped") {
    return createLinkMetric(aNode, zNode,
                            "RX dropped", "Receive packets/second",
                            "rx_dropped", "link");
  }
  else if (metricName == "tx_pps") {
    return createLinkMetric(aNode, zNode,
                            "TX pps", "Transmit packets/second",
                            "tc_packets", "link");
  }
  else if (metricName == "rx_pps") {
    return createLinkMetric(aNode, zNode,
                            "RX pps", "Receive packets/second",
                            "rc_packets", "link");
  }
  else if (metricName == "tx_power") {
    return createLinkMetric(aNode, zNode,
                            "TX Power", "Transmit Power",
                            "tpcStats.txPowerIndex");
  }
  else if (metricName == "rx_frame") {
    return createLinkMetric(aNode, zNode,
                            "RX Frame", "RX Frame",
                            "rx_frame", "link");
  }
  else if (metricName == "rx_overruns") {
    return createLinkMetric(aNode, zNode,
                            "RX Overruns", "RX Overruns",
                            "rx_overruns", "link");
  }
  else if (metricName == "tx_overruns") {
    return createLinkMetric(aNode, zNode,
                            "TX Overruns", "TX Overruns",
                            "tx_overruns", "link");
  }
  else if (metricName == "tx_collisions") {
    return createLinkMetric(aNode, zNode,
                            "TX Collisions", "TX Collisions",
                            "tx_collisions", "link");
  }
  else if (metricName == "speed") {
    return createLinkMetric(aNode, zNode,
                            "Speed", "Speed (mbps)",
                            "speed", "link");
  }
  else if (metricName == "link_status") {
    return folly::dynamic::object
      ("title", "Link status")
      ("description", "Link status reported by controller")
      ("scale", NULL)
      ("keys", folly::dynamic::array(
        folly::dynamic::object
          ("node", SimpleJSONSerializer::serialize<std::string>(aNode))
          ("keyName", "e2e_controller.link_status.WIRELESS." + aNode.mac_addr + "." + zNode.mac_addr)
          ("titleAppend", " (A)")));
  }
}

void StatsTypeAheadHandler::onEOM() noexcept {
  auto body = body_->moveToFbString();
  query::Topology request;
  try {
    request = SimpleJSONSerializer::deserialize<query::Topology>(body);
  } catch (const std::exception& ex) {
    LOG(INFO) << "Error deserializing stats type ahead request";
    ResponseBuilder(downstream_)
        .status(500, "OK")
        .header("Content-Type", "application/json")
        .body("Failed de-serializing stats type ahead request")
        .sendWithEOM();
    return;
  }
  LOG(INFO) << "Stats type ahead request from \"" << request.name;

  try {
    fetchMetricNames(request);
  } catch (const std::exception& ex) {
    LOG(ERROR) << "Unable to handle stats_type_ahead request: " << ex.what();
    ResponseBuilder(downstream_)
        .status(500, "OK")
        .header("Content-Type", "application/json")
        .body("Failed handling stats_type_ahead request")
        .sendWithEOM();
    return;
  }
  ResponseBuilder(downstream_)
      .status(200, "OK")
      .header("Content-Type", "application/json")
      .body(folly::toJson(siteMetrics_))
      .sendWithEOM();
}

void StatsTypeAheadHandler::onUpgrade(UpgradeProtocol /* unused */) noexcept {}

void StatsTypeAheadHandler::requestComplete() noexcept {
  delete this;
}

void StatsTypeAheadHandler::onError(ProxygenError /* unused */) noexcept {
  LOG(ERROR) << "Proxygen reported error";
  // In QueryServiceFactory, we created this handler using new.
  // Proxygen does not delete the handler.
  delete this;
}

}
} // facebook::gorilla
