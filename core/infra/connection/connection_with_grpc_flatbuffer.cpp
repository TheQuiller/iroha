
/*
Copyright Soramitsu Co., Ltd. 2016 All Rights Reserved.
Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at
         http://www.apache.org/licenses/LICENSE-2.0
Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/


#include <grpc++/grpc++.h>
#include <service/flatbuffer_service.h>

#include <crypto/hash.hpp>
#include <crypto/signature.hpp>
#include <infra/config/iroha_config_with_json.hpp>
#include <infra/config/peer_service_with_json.hpp>
#include <membership_service/peer_service.hpp>
#include <service/connection.hpp>
#include <utils/datetime.hpp>
#include <utils/expected.hpp>
#include <utils/logger.hpp>

#include <endpoint.grpc.fb.h>
#include <main_generated.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace connection {
/**
 * Using
 */
using Sumeragi = ::iroha::Sumeragi;
using Hijiri = ::iroha::Hijiri;
using ConsensusEvent = ::iroha::ConsensusEvent;
using Ping = ::iroha::Ping;
using Response = ::iroha::Response;
using Transaction = ::iroha::Transaction;
using TransactionWrapper = ::iroha::TransactionWrapper;
using Signature = ::iroha::Signature;
using Sync = ::iroha::Sync;

using grpc::Channel;
using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ClientContext;
using grpc::Status;

/**
 * Enum
 */
enum ResponseType {
  RESPONSE_OK,
  RESPONSE_INVALID_SIG,  // wrong signature
  RESPONSE_ERRCONN,      // connection error
};


/************************************************************************************
 * Interface: Verify, Torii :: receive()
 ************************************************************************************/
template <class CallBackFunc>
class Receiver {
 public:
  VoidHandler set(CallBackFunc &&rhs) {
    if (receiver_) {
      return makeUnexpected(exception::DuplicateSetArgumentException(
          "Receiver<" + std::string(typeid(CallBackFunc).name()) + ">",
          __FILE__));
    }

    receiver_ = std::make_shared<CallBackFunc>(rhs);
    return {};
  }

  // ToDo rewrite operator() overload.
  void invoke(const std::string &from, flatbuffers::unique_ptr_t &&arg) {
    (*receiver_)(from, std::move(arg));
  }

 private:
  std::shared_ptr<CallBackFunc> receiver_;
};

/**
 * Verify
 */
namespace iroha {
namespace SumeragiImpl {
namespace Verify {
Receiver<Verify::CallBackFunc> receiver;

void receive(Verify::CallBackFunc &&callback) {
  receiver.set(std::move(callback));
}

}  // namespace Verify
}  // namespace SumeragiImpl
}  // namespace iroha

/**
 * Torii
 */
namespace iroha {
namespace SumeragiImpl {
namespace Torii {
Receiver<Torii::CallBackFunc> receiver;

void receive(Torii::CallBackFunc &&callback) {
  receiver.set(std::move(callback));
}

}  // namespace Torii
}  // namespace SumeragiImpl
}  // namespace iroha

/************************************************************************************
 * RPC: Verify, Torii
 ************************************************************************************/
/**
 * SumeragiConnectionClient
 * - Verify, Torii
 */
class SumeragiConnectionClient {
 public:
  explicit SumeragiConnectionClient(std::shared_ptr<Channel> channel)
      : stub_(Sumeragi::NewStub(channel)) {}

  void Verify(const ::iroha::ConsensusEvent &consensusEvent,
              flatbuffers::BufferRef<Response> *responseRef) const {
    ClientContext context;
    logger::info("connection") << "Operation";
    logger::info("connection")
        << "size: " << consensusEvent.peerSignatures()->size();
    logger::info("connection")
        << "Transaction: "
        << flatbuffer_service::toString(
               *consensusEvent.transactions()->Get(0)->tx_nested_root());

    flatbuffers::FlatBufferBuilder fbb;

    auto eventOffset =
        flatbuffer_service::copyConsensusEvent(fbb, consensusEvent);

    if (!eventOffset) {
      // FIXME: Use Response
      assert(false);
    }

    fbb.Finish(*eventOffset);

    auto reqEventRef = flatbuffers::BufferRef<::iroha::ConsensusEvent>(
        fbb.GetBufferPointer(), fbb.GetSize());

    Status status = stub_->Verify(&context, reqEventRef, responseRef);

    if (status.ok()) {
      logger::info("SumeragiConnectionClient::Verify")
          << "response: " << responseRef->GetRoot()->message()->c_str();
    } else {
      logger::error("SumeragiConnectionClient::Verify")
          << static_cast<int>(status.error_code()) << ": "
          << status.error_message();
      logger::error("SumeragiConnectionClient::Verify")
          << responseRef->GetRoot()->message()->c_str();
    }
  }

  void Torii(const ::iroha::Transaction &tx,
             flatbuffers::BufferRef<Response> *responseRef) const {
    // Copy transaction to FlatBufferBuilder memory, then create
    // BufferRef<Transaction>
    // and share it to another sumeragi by using stub interface Torii.
    logger::info("connection") << "Operation";
    logger::info("connection") << "tx: " << flatbuffer_service::toString(tx);

    ::grpc::ClientContext clientContext;
    flatbuffers::FlatBufferBuilder xbb;

    auto txOffset = flatbuffer_service::copyTransaction(xbb, tx);
    if (!txOffset) {
      assert(false);
    }

    xbb.Finish(txOffset.value());

    flatbuffers::BufferRef<::iroha::Transaction> requestTxRef(
        xbb.GetBufferPointer(), xbb.GetSize());

    Status status = stub_->Torii(&clientContext, requestTxRef, responseRef);

    if (status.ok()) {
      logger::info("SumeragiConnectionClient::Torii") << "gRPC OK";
    } else {
      logger::error("SumeragiConnectionClient::Torii")
          << "gRPC CANCELLED" << static_cast<int>(status.error_code()) << ": "
          << status.error_message();
    }
  }

 private:
  std::unique_ptr<Sumeragi::Stub> stub_;
};

/**
 * SumeragiConnectionServiceImpl
 */
class SumeragiConnectionServiceImpl final : public ::iroha::Sumeragi::Service {
 public:
  Status Verify(ServerContext *context,
                const flatbuffers::BufferRef<ConsensusEvent> *request,
                flatbuffers::BufferRef<Response> *response) override {
    fbbResponse.Clear();

    {
      flatbuffers::FlatBufferBuilder fbb;
      auto event =
          flatbuffer_service::copyConsensusEvent(fbb, *request->GetRoot());
      if (!event) {
        fbb.Clear();
        auto responseOffset = ::iroha::CreateResponseDirect(
            fbbResponse, "CANCELLED", ::iroha::Code::FAIL,
            0);  // FIXME: Currently, if it fails, no signature.
        fbbResponse.Finish(responseOffset);

        *response = flatbuffers::BufferRef<::iroha::Response>(
            fbbResponse.GetBufferPointer(), fbbResponse.GetSize());
        return Status::CANCELLED;
      }

      fbb.Finish(event.value());

      const std::string from = "from";
      connection::iroha::SumeragiImpl::Verify::receiver.invoke(
          from, std::move(fbb.ReleaseBufferPointer()));
    }

    auto tx_str =
        flatbuffer_service::toString(*request->GetRoot()
                                          ->transactions()
                                          ->Get(0)  // Future work: #(tx) = 1
                                          ->tx_nested_root());
    auto responseOffset = ::iroha::CreateResponseDirect(
        fbbResponse, "OK!!", ::iroha::Code::UNDECIDED,
        flatbuffer_service::primitives::CreateSignature(fbbResponse, tx_str,
                                                        datetime::unixtime()));

    fbbResponse.Finish(responseOffset);

    *response = flatbuffers::BufferRef<::iroha::Response>(
        fbbResponse.GetBufferPointer(), fbbResponse.GetSize());

    return Status::OK;
  }

  Status Torii(ServerContext *context,
               const flatbuffers::BufferRef<Transaction> *txRef,
               flatbuffers::BufferRef<Response> *responseRef) override {
    logger::debug("SumeragiConnectionServiceImpl::Torii") << "RPC works";

    // This reference remains until next calling
    // SumeragiConnectionServiceImpl::Torii() method.
    fbbResponse.Clear();

    {
      const auto tx = txRef->GetRoot();
      flatbuffers::FlatBufferBuilder fbb;
      auto txoffset = flatbuffer_service::copyTransaction(fbb, *tx);
      if (!txoffset) {
        fbb.Clear();
        auto responseOffset = ::iroha::CreateResponseDirect(
            fbbResponse, "CANCELLED", ::iroha::Code::FAIL,
            0);  // FIXME: Currently, if it fails, no signature.
        fbbResponse.Finish(responseOffset);

        *responseRef = flatbuffers::BufferRef<Response>(
            fbbResponse.GetBufferPointer(), fbbResponse.GetSize());
        return Status::CANCELLED;
      }

      fbb.Finish(txoffset.value());
      connection::iroha::SumeragiImpl::Torii::receiver.invoke(
          "from",  // TODO: Specify 'from'
          fbb.ReleaseBufferPointer());
    }

    auto tx_str = flatbuffer_service::toString(*txRef->GetRoot());

    auto responseOffset = ::iroha::CreateResponseDirect(
        fbbResponse, "OK!!", ::iroha::Code::UNDECIDED,
        flatbuffer_service::primitives::CreateSignature(fbbResponse, tx_str,
                                                        datetime::unixtime()));

    fbbResponse.Finish(responseOffset);

    *responseRef = flatbuffers::BufferRef<Response>(
        fbbResponse.GetBufferPointer(), fbbResponse.GetSize());

    return Status::OK;
  }

 private:
  flatbuffers::FlatBufferBuilder fbbResponse;
};


/************************************************************************************
 * Interface: Verify::send()
 ************************************************************************************/
namespace iroha {
namespace SumeragiImpl {
namespace Verify {
bool send(const std::string &ip, const ::iroha::ConsensusEvent &event) {
  logger::info("connection") << "Send!";
  if (::peer::service::isExistIP(ip)) {
    logger::info("connection") << "IP exists: " << ip;
    SumeragiConnectionClient client(grpc::CreateChannel(
        ip + ":" +
            std::to_string(config::IrohaConfigManager::getInstance()
                               .getGrpcPortNumber(50051)),
        grpc::InsecureChannelCredentials()));
    // TODO return tx validity
    flatbuffers::BufferRef<::iroha::Response> response;
    client.Verify(event, &response);
    auto reply = response.GetRoot();
    if (reply->code() == ::iroha::Code::FAIL) {
      logger::error("connection")
          << ::iroha::EnumNameCode(reply->code()) << ", " << reply->message();
      return false;
    }
    return true;
  } else {
    logger::info("connection") << "IP doesn't exist: " << ip;
    return false;
  }
}

bool sendAll(const ::iroha::ConsensusEvent &event) {
  auto receiver_ips = config::PeerServiceConfig::getInstance().getGroup();
  for (const auto &p : receiver_ips) {
    if (p["ip"].get<std::string>() !=
        config::PeerServiceConfig::getInstance().getMyIp()) {
      logger::info("connection") << "Send to " << p["ip"].get<std::string>();
      send(p["ip"].get<std::string>(), event);
    }
  }
  return true;
}

}  // namespace Verify
}  // namespace SumeragiImpl
}  // namespace iroha

/************************************************************************************
 * Hijiri
 ************************************************************************************/
class HijiriConnectionClient {
 public:
  explicit HijiriConnectionClient(std::shared_ptr<Channel> channel)
      : stub_(Hijiri::NewStub(channel)) {}

  void Kagami(const ::iroha::Ping &ping,
              flatbuffers::BufferRef<Response> *responseRef) const {
    ::grpc::ClientContext clientContext;
    flatbuffers::FlatBufferBuilder fbbPing;

    auto pingOffset = ::iroha::CreatePingDirect(
        fbbPing, ping.message()->c_str(), ping.sender()->c_str());
    fbbPing.Finish(pingOffset);

    flatbuffers::BufferRef<::iroha::Ping> reqPingRef(fbbPing.GetBufferPointer(),
                                                     fbbPing.GetSize());

    auto res = stub_->Kagami(&clientContext, reqPingRef, responseRef);
    ;
    logger::info("connection") << "Send!";

    if (res.ok()) {
      logger::info("HijiriConnectionClient") << "gRPC OK";
    } else {
      logger::error("HijiriConnectionClient")
          << "gRPC CANCELLED" << static_cast<int>(res.error_code()) << ": "
          << res.error_message();
    }
  }

 private:
  std::unique_ptr<Hijiri::Stub> stub_;
};

class HijiriConnectionServiceImpl final : public ::iroha::Hijiri::Service {
 public:
  Status Kagami(ServerContext *context,
                const flatbuffers::BufferRef<Ping> *request,
                flatbuffers::BufferRef<Response> *responseRef) override {
    fbbResponse.Clear();
    {
      auto responseOffset = ::iroha::CreateResponseDirect(
          fbbResponse, "OK!!", ::iroha::Code::UNDECIDED,
          sign(fbbResponse,
               hash::sha3_256_hex(request->GetRoot()->message()->str() +
                                  request->GetRoot()->message()->str())));
      fbbResponse.Finish(responseOffset);

      *responseRef = flatbuffers::BufferRef<Response>(
          fbbResponse.GetBufferPointer(), fbbResponse.GetSize());

      return Status::OK;
    }
  }

  // ToDo: Unite the way to hash (below double(?) hash)
  flatbuffers::Offset<::iroha::Signature> sign(
      flatbuffers::FlatBufferBuilder &fbb, const std::string &tx) {
    const auto stamp = datetime::unixtime();
    const auto hashWithTimestamp =
        hash::sha3_256_hex(tx + std::to_string(stamp));
    const auto signature = signature::sign(
        hashWithTimestamp,
        config::PeerServiceConfig::getInstance().getMyPublicKey(),
        config::PeerServiceConfig::getInstance().getMyPrivateKey());
    const std::vector<uint8_t> sigblob(signature.begin(), signature.end());
    return ::iroha::CreateSignatureDirect(
        fbb, config::PeerServiceConfig::getInstance().getMyPublicKey().c_str(),
        &sigblob, stamp);
  };

 private:
  flatbuffers::FlatBufferBuilder fbbResponse;
};

namespace memberShipService {
namespace HijiriImpl {
namespace Kagami {
bool send(const std::string &ip, const ::iroha::Ping &ping) {  // TODO
  logger::info("connection") << "Send!";
  logger::info("connection") << "IP is: " << ip;
  HijiriConnectionClient client(grpc::CreateChannel(
      ip + ":" +
          std::to_string(config::IrohaConfigManager::getInstance()
                             .getGrpcPortNumber(50051)),
      grpc::InsecureChannelCredentials()));

  flatbuffers::BufferRef<Response> response;
  client.Kagami(ping, &response);
  auto reply = response.GetRoot();
  return true;
}
}  // namespace Kagami
}  // namespace HijiriImpl
namespace SumeragiImpl {
namespace Torii {
bool send(const std::string &ip, const ::iroha::Transaction &tx) {
  logger::info("connection") << "Send!";
  if (::peer::service::isExistIP(ip)) {
    logger::info("connection") << "IP Exist: " << ip;
    SumeragiConnectionClient client(grpc::CreateChannel(
        ip + ":" +
            std::to_string(config::IrohaConfigManager::getInstance()
                               .getGrpcPortNumber(50051)),
        grpc::InsecureChannelCredentials()));

    flatbuffers::BufferRef<Response> response;
    client.Torii(tx, &response);
    auto reply = response.GetRoot();
    return true;
  }
}
}  // namespace Torii
}  // namespace SumeragiImpl
}  // namespace memberShipService

/************************************************************************************
 * Sync
 ************************************************************************************/

class SyncConnectionClient {
 public:
  explicit SyncConnectionClient(std::shared_ptr<Channel> channel)
      : stub_(Sync::NewStub(channel)) {}

  std::vector<uint8_t> checkHash(
    const ::iroha::Ping &ping
  ) const {
    ::grpc::ClientContext clientContext;
    flatbuffers::FlatBufferBuilder fbbPing;

    auto pingOffset = ::iroha::CreatePingDirect(
        fbbPing, ping.message()->c_str(), ping.sender()->c_str());
    fbbPing.Finish(pingOffset);

    flatbuffers::BufferRef<::iroha::Ping> reqPingRef(fbbPing.GetBufferPointer(),
                                                     fbbPing.GetSize());

    flatbuffers::BufferRef<::iroha::CheckHashResponse> responseRef;
    auto res = stub_->checkHash(&clientContext, reqPingRef, &responseRef);

    logger::info("Connection with grpc") << "Send!";

    if (res.ok()) {
      logger::info("connection")
          << "response: " << responseRef.GetRoot()->isCorrect();
      return {responseRef.buf, responseRef.buf + responseRef.len};
    } else {
      logger::error("connection")
          << static_cast<int>(res.error_code()) << ": " << res.error_message();
      // std::cout << status.error_code() << ": " << status.error_message();
      return {};
    }
  }

  std::vector<uint8_t> getPeers(
    const ::iroha::Ping &ping
  ) const {
    ::grpc::ClientContext clientContext;
    flatbuffers::FlatBufferBuilder fbbPing;

    auto pingOffset = ::iroha::CreatePingDirect(
        fbbPing, ping.message()->c_str(), ping.sender()->c_str());
    fbbPing.Finish(pingOffset);

    flatbuffers::BufferRef<::iroha::Ping> reqPingRef(fbbPing.GetBufferPointer(),
                                                     fbbPing.GetSize());

    flatbuffers::BufferRef<::iroha::PeersResponse> responseRef;

    auto res = stub_->getPeers(&clientContext, reqPingRef, &responseRef);
    logger::info("Connection with grpc") << "Send!";

    if (res.ok()) {
      logger::info("connection")
          << "response: " << responseRef.GetRoot()->message();
      return {responseRef.buf, responseRef.buf + responseRef.len};
    } else {
      logger::error("connection")
          << static_cast<int>(res.error_code()) << ": " << res.error_message();
      // std::cout << status.error_code() << ": " << status.error_message();
      return {};
    }
  }

 private:
  std::unique_ptr<Sync::Stub> stub_;
};

class SyncConnectionServiceImpl final : public ::iroha::Sync::Service {
 public:
  Status checkHash(ServerContext *context,
                   const flatbuffers::BufferRef<Ping> *request,
                   flatbuffers::BufferRef<::iroha::CheckHashResponse>
                       *responseRef) override {
    fbbResponse.Clear();
    {
      std::string hash = request->GetRoot()->message()->str();
      // Now, only supported root hash copare. (ver1.0)
      if (true) {  // TODO unused it yet? repository::getMerkleRoot() ==
                   // hash) {
        auto responseOffset =
            ::iroha::CreateCheckHashResponse(fbbResponse, true, true, true);
        fbbResponse.Finish(responseOffset);
      } else {
        auto responseOffset =
            ::iroha::CreateCheckHashResponse(fbbResponse, false, false, false);
        fbbResponse.Finish(responseOffset);
      }
      *responseRef = flatbuffers::BufferRef<::iroha::CheckHashResponse>(
          fbbResponse.GetBufferPointer(), fbbResponse.GetSize());
      return Status::OK;
    }
  }

  Status getPeers(
      ServerContext *context, const flatbuffers::BufferRef<Ping> *request,
      flatbuffers::BufferRef<::iroha::PeersResponse> *responseRef) override {
    fbbResponse.Clear();
    {
      std::string leader_ip = request->GetRoot()->message()->str();
      std::vector<flatbuffers::Offset<::iroha::Peer>> p_vec;
      for (auto &&p : ::peer::service::getAllPeerList()) {
        flatbuffers::FlatBufferBuilder tbb;
        p_vec.emplace_back(::iroha::CreatePeer(
            tbb, tbb.CreateString(p->ledger_name),
            tbb.CreateString(p->publicKey), tbb.CreateString(p->ip), p->trust,
            p->active, p->join_ledger));
      }
      auto res = ::iroha::CreatePeersResponse(
          fbbResponse, fbbResponse.CreateString("message"),
          fbbResponse.CreateVector(p_vec),
          fbbResponse.CreateString(::peer::myself::getPublicKey()));
      fbbResponse.Finish(res);
      *responseRef = flatbuffers::BufferRef<::iroha::PeersResponse>(
          fbbResponse.GetBufferPointer(), fbbResponse.GetSize());
      return Status::OK;
    }
  }

 private:
  flatbuffers::FlatBufferBuilder fbbResponse;
};

namespace memberShipService {
namespace SyncImpl {
namespace checkHash {
bool send(const std::string &ip, const ::iroha::Ping &ping) {
  logger::info("Connection with grpc") << "Send!";
  if (::peer::service::isExistIP(ip)) {
    logger::info("Connection with grpc") << "IP exists: " << ip;
    SyncConnectionClient client(grpc::CreateChannel(
        ip + ":" +
            std::to_string(config::IrohaConfigManager::getInstance()
                               .getGrpcPortNumber(50051)),
        grpc::InsecureChannelCredentials()));

    flatbuffers::BufferRef<::iroha::CheckHashResponse> responseRef;
    auto replyvec = client.checkHash(ping);
    auto reply = flatbuffers::GetRoot<::iroha::CheckHashResponse>(replyvec.data());
    return reply->isCorrect();
  } else
    return false;
}
}  // namespace checkHash

namespace getPeers {
bool send(const std::string &ip, const ::iroha::Ping &ping) {
  logger::info("Connection with grpc") << "Send!";
  logger::info("Connection with grpc") << "IP exists: " << ip;
  SyncConnectionClient client(grpc::CreateChannel(
      ip + ":" +
          std::to_string(config::IrohaConfigManager::getInstance()
                             .getGrpcPortNumber(50051)),
      grpc::InsecureChannelCredentials()));
  flatbuffers::BufferRef<::iroha::PeersResponse> responseRef;

  auto replyvec = client.getPeers(ping);
  auto reply = flatbuffers::GetRoot<::iroha::PeersResponse>(replyvec.data());

  for (auto it = reply->peers()->begin(); it != reply->peers()->end(); it++) {
    std::cout << "ip: " << it->ip()->c_str() << std::endl;
    std::cout << "pubkey: " << it->publicKey()->c_str() << std::endl;
    std::cout << "leadger: " << it->ledger_name()->c_str() << std::endl;
    std::string ip = it->ip()->c_str();
    std::string pubkey = it->publicKey()->c_str();
    std::string lerger = it->ledger_name()->c_str();
    auto p = ::peer::Node(ip, pubkey, lerger, it->trust(), it->active(),
                          it->join_ledger());
    if (::peer::transaction::validator::add(p))
      ::peer::transaction::executor::add(p);
  }
  return true;
}
}  // namespace getPeers
}  // namespace SyncImpl
}  // namespace memberShipService

/************************************************************************************
 * Run server
 ************************************************************************************/
std::mutex wait_for_server;
ServerBuilder builder;
grpc::Server *server = nullptr;
std::condition_variable server_cv;
bool server_ready = false;

void initialize_peer() {
  // ToDo catch exception of to_string


  logger::info("Connection GRPC") << " initialize_peer ";
}

void wait_till_ready() {
  std::unique_lock<std::mutex> lk(wait_for_server);
  server_cv.wait(lk, [] { return server_ready; });
}

int run() {
  logger::info("Connection GRPC") << " RUN ";
  auto address =
      "0.0.0.0:" +
      std::to_string(
          config::IrohaConfigManager::getInstance().getGrpcPortNumber(50051));
  SumeragiConnectionServiceImpl service;
  SyncConnectionServiceImpl service_sync;
  grpc::ServerBuilder builder;
  builder.AddListeningPort(address, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);
  builder.RegisterService(&service_sync);  // TODO WIP Is it OK?

  {
    std::lock_guard<std::mutex> lk(wait_for_server);
    // wait_for_server.lock();
    server = builder.BuildAndStart().release();
    server_ready = true;
    // wait_for_server.unlock();
  }
  server_cv.notify_one();

  server->Wait();
  return 0;
}

void finish() {
  server->Shutdown();
  delete server;
}
}  // namespace connection