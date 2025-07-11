#pragma once

#include <Functions/MatchImpl.h>
#include <Functions/FunctionsStringSearch.h>

namespace DB
{

struct NameLike
{
    static constexpr auto name = "like";
};

using LikeImpl = MatchImpl<NameLike, MatchTraits::Syntax::Like, MatchTraits::Case::Sensitive, MatchTraits::Result::DontNegate>;
using FunctionLike = FunctionsStringSearch<LikeImpl>;

}
