/**
 * Copyright Soramitsu Co., Ltd. 2017 All Rights Reserved.
 * http://soramitsu.co.jp
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef IROHA_TEMPORARY_WSV_IMPL_HPP
#define IROHA_TEMPORARY_WSV_IMPL_HPP

#include <pqxx/connection>
#include <pqxx/nontransaction>
#include <soci/soci.h>

#include "ametsuchi/temporary_wsv.hpp"
#include "execution/command_executor.hpp"
#include "logger/logger.hpp"

namespace iroha {

  namespace ametsuchi {
    class TemporaryWsvImpl : public TemporaryWsv {
     public:
      TemporaryWsvImpl(std::unique_ptr<soci::session> sql);

      bool apply(
          const shared_model::interface::Transaction &,
          std::function<bool(const shared_model::interface::Transaction &,
                             WsvQuery &)> function) override;

      ~TemporaryWsvImpl() override;

      std::unique_ptr<soci::session> sql_;
     private:
      std::shared_ptr<WsvQuery> wsv_;
      std::shared_ptr<WsvCommand> executor_;
      std::shared_ptr<CommandExecutor> command_executor_;
      std::shared_ptr<CommandValidator> command_validator_;

      logger::Logger log_;
    };
  }  // namespace ametsuchi
}  // namespace iroha

#endif  // IROHA_TEMPORARY_WSV_IMPL_HPP
