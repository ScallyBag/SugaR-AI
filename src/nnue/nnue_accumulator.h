/*
  SugaR, a UCI chess playing engine derived from Stockfish
  Copyright (C) 2004-2021 The Stockfish developers (see AUTHORS file)

  SugaR is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  SugaR is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// Class for difference calculation of NNUE evaluation function

#ifndef NNUE_ACCUMULATOR_H_INCLUDED
#define NNUE_ACCUMULATOR_H_INCLUDED

#include "nnue_architecture.h"

namespace Stockfish::Eval::NNUE {

  // The accumulator of a StateInfo without parent is set to the INIT state
  enum AccumulatorState { EMPTY, COMPUTED, INIT };

  // Class that holds the result of affine transformation of input features
  struct alignas(CacheLineSize) Accumulator {
    std::int16_t accumulation[2][TransformedFeatureDimensions];
    std::int32_t psqtAccumulation[2][PSQTBuckets];
    AccumulatorState state[2];
  };

}  // namespace Stockfish::Eval::NNUE

#endif // NNUE_ACCUMULATOR_H_INCLUDED
