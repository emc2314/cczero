/*
  This file is part of Chinese Chess Zero.
  Copyright (C) 2018 The CCZero Authors

  Chinese Chess Zero is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Chinese Chess Zero is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Chinese Chess Zero.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <algorithm>
#include <iostream>

#include "neural/factory.h"

namespace cczero {

NetworkFactory* NetworkFactory::Get() {
    static NetworkFactory factory;
    return &factory;
}

NetworkFactory::Register::Register(const std::string& name, FactoryFunc factory,
                                   int priority) {
    NetworkFactory::Get()->RegisterNetwork(name, factory, priority);
}

void NetworkFactory::RegisterNetwork(const std::string& name,
                                     FactoryFunc factory, int priority) {
    factories_.emplace_back(name, factory, priority);
    std::sort(factories_.begin(), factories_.end());
}

std::vector<std::string> NetworkFactory::GetBackendsList() const {
    std::vector<std::string> result;
    for (const auto& x : factories_) result.emplace_back(x.name);
    return result;
}

std::unique_ptr<Network> NetworkFactory::Create(const std::string& network,
                                                const Weights& weights,
                                                const OptionsDict& options) {
    std::cerr << "Creating backend [" << network << "]..." << std::endl;
    for (const auto& factory : factories_) {
        if (factory.name == network) {
            return factory.factory(weights, options);
        }
    }
    throw Exception("Unknown backend: " + network);
}

}  // namespace cczero