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

#pragma once

#include <thread>

#include "chess/uciloop.h"
#include "selfplay/tournament.h"
#include "utils/optionsparser.h"

namespace cczero {

class SelfPlayLoop : public UciLoop {
   public:
    SelfPlayLoop();
    ~SelfPlayLoop();

    void RunLoop() override;
    void CmdStart() override;
    void CmdUci() override;
    void CmdSetOption(const std::string& name, const std::string& value,
                      const std::string& context) override;

   private:
    void SendGameInfo(const GameInfo& move);
    void SendTournament(const TournamentInfo& info);

    void EnsureOptionsSent();
    OptionsParser options_;

    std::unique_ptr<SelfPlayTournament> tournament_;
    std::unique_ptr<std::thread> thread_;
};

}  // namespace cczero