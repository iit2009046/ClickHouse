#include <Functions/FunctionFactory.h>
#include <Functions/FunctionBinaryArithmetic.h>

#include <libdivide-config.h>
#include <libdivide.h>


namespace DB
{
namespace ErrorCodes
{
    extern const int ILLEGAL_DIVISION;
}

namespace
{

/// Optimizations for integer modulo by a constant.

template <typename A, typename B>
struct ModuloByConstantImpl
    : BinaryOperation<A, B, ModuloImpl<A, B>>
{
    using Op = ModuloImpl<A, B>;
    using ResultType = typename Op::ResultType;
    static const constexpr bool allow_fixed_string = false;
    static const constexpr bool allow_string_integer = false;

    template <OpCase op_case>
    static void NO_INLINE process(const A * __restrict a, const B * __restrict b, ResultType * __restrict c, size_t size, const NullMap * right_nullmap)
    {
        if constexpr (op_case == OpCase::RightConstant)
        {
            if (right_nullmap && (*right_nullmap)[0])
                return;
            vectorConstant(a, *b, c, size);
        }
        else
        {
            if (right_nullmap)
            {
                for (size_t i = 0; i < size; ++i)
                    if ((*right_nullmap)[i])
                        c[i] = ResultType();
                    else
                        apply<op_case>(a, b, c, i);
            }
            else
                for (size_t i = 0; i < size; ++i)
                    apply<op_case>(a, b, c, i);
        }
    }

    static ResultType process(A a, B b) { return Op::template apply<ResultType>(a, b); }

    static void NO_INLINE NO_SANITIZE_UNDEFINED vectorConstant(const A * __restrict src, B b, ResultType * __restrict dst, size_t size)
    {
        /// Modulo with too small divisor.
        if (unlikely((std::is_signed_v<B> && b == -1) || b == 1))
        {
            for (size_t i = 0; i < size; ++i)
                dst[i] = 0;
            return;
        }

        /// Modulo with too large divisor.
        if (unlikely(b > std::numeric_limits<A>::max()
            || (std::is_signed_v<A> && std::is_signed_v<B> && b < std::numeric_limits<A>::lowest())))
        {
            for (size_t i = 0; i < size; ++i)
                dst[i] = static_cast<ResultType>(src[i]);
            return;
        }

        if (unlikely(static_cast<A>(b) == 0))
            throw Exception(ErrorCodes::ILLEGAL_DIVISION, "Division by zero");

        /// Division by min negative value.
        if (std::is_signed_v<B> && b == std::numeric_limits<B>::lowest())
            throw Exception(ErrorCodes::ILLEGAL_DIVISION, "Division by the most negative number");

        /// Modulo of division by negative number is the same as the positive number.
        if (b < 0)
            b = -b;

        /// Here we failed to make the SSE variant from libdivide give an advantage.

        if (b & (b - 1))
        {
            libdivide::divider<A> divider(static_cast<A>(b));
            for (size_t i = 0; i < size; ++i)
            {
                /// NOTE: perhaps, the division semantics with the remainder of negative numbers is not preserved.
                dst[i] = static_cast<ResultType>(src[i] - (src[i] / divider) * b);
            }
        }
        else
        {
            // gcc libdivide doesn't work well for pow2 division
            auto mask = b - 1;
            for (size_t i = 0; i < size; ++i)
                dst[i] = static_cast<ResultType>(src[i] & mask);
        }
    }

private:
    template <OpCase op_case>
    static void apply(const A * __restrict a, const B * __restrict b, ResultType * __restrict c, size_t i)
    {
        if constexpr (op_case == OpCase::Vector)
            c[i] = Op::template apply<ResultType>(a[i], b[i]);
        else
            c[i] = Op::template apply<ResultType>(*a, b[i]);
    }
};

template <typename A, typename B>
struct ModuloLegacyByConstantImpl : ModuloByConstantImpl<A, B>
{
    using Op = ModuloLegacyImpl<A, B>;
};

}

/** Specializations are specified for dividing numbers of the type UInt64 and UInt32 by the numbers of the same sign.
  * Can be expanded to all possible combinations, but more code is needed.
  */

namespace impl_
{
template <> struct BinaryOperationImpl<UInt64, UInt8, ModuloImpl<UInt64, UInt8>> : ModuloByConstantImpl<UInt64, UInt8> {};
template <> struct BinaryOperationImpl<UInt64, UInt16, ModuloImpl<UInt64, UInt16>> : ModuloByConstantImpl<UInt64, UInt16> {};
template <> struct BinaryOperationImpl<UInt64, UInt32, ModuloImpl<UInt64, UInt32>> : ModuloByConstantImpl<UInt64, UInt32> {};
template <> struct BinaryOperationImpl<UInt64, UInt64, ModuloImpl<UInt64, UInt64>> : ModuloByConstantImpl<UInt64, UInt64> {};

template <> struct BinaryOperationImpl<UInt32, UInt8, ModuloImpl<UInt32, UInt8>> : ModuloByConstantImpl<UInt32, UInt8> {};
template <> struct BinaryOperationImpl<UInt32, UInt16, ModuloImpl<UInt32, UInt16>> : ModuloByConstantImpl<UInt32, UInt16> {};
template <> struct BinaryOperationImpl<UInt32, UInt32, ModuloImpl<UInt32, UInt32>> : ModuloByConstantImpl<UInt32, UInt32> {};
template <> struct BinaryOperationImpl<UInt32, UInt64, ModuloImpl<UInt32, UInt64>> : ModuloByConstantImpl<UInt32, UInt64> {};

template <> struct BinaryOperationImpl<Int64, Int8, ModuloImpl<Int64, Int8>> : ModuloByConstantImpl<Int64, Int8> {};
template <> struct BinaryOperationImpl<Int64, Int16, ModuloImpl<Int64, Int16>> : ModuloByConstantImpl<Int64, Int16> {};
template <> struct BinaryOperationImpl<Int64, Int32, ModuloImpl<Int64, Int32>> : ModuloByConstantImpl<Int64, Int32> {};
template <> struct BinaryOperationImpl<Int64, Int64, ModuloImpl<Int64, Int64>> : ModuloByConstantImpl<Int64, Int64> {};

template <> struct BinaryOperationImpl<Int32, Int8, ModuloImpl<Int32, Int8>> : ModuloByConstantImpl<Int32, Int8> {};
template <> struct BinaryOperationImpl<Int32, Int16, ModuloImpl<Int32, Int16>> : ModuloByConstantImpl<Int32, Int16> {};
template <> struct BinaryOperationImpl<Int32, Int32, ModuloImpl<Int32, Int32>> : ModuloByConstantImpl<Int32, Int32> {};
template <> struct BinaryOperationImpl<Int32, Int64, ModuloImpl<Int32, Int64>> : ModuloByConstantImpl<Int32, Int64> {};
}

struct NameModulo { static constexpr auto name = "modulo"; };
using FunctionModulo = BinaryArithmeticOverloadResolver<ModuloImpl, NameModulo, false>;

REGISTER_FUNCTION(Modulo)
{
    FunctionDocumentation::Description description = R"(
    Calculates the remainder of the division of two values a by b.

    The result type is an integer if both inputs are integers. If one of the
    inputs is a floating-point number, the result type is Float64.

    The remainder is computed like in C++. Truncated division is used for
    negative numbers.

    An exception is thrown when dividing by zero or when dividing a minimal
    negative number by minus one.
    )";
    FunctionDocumentation::Syntax syntax = "modulo(a, b)";
    FunctionDocumentation::Argument argument1 = {"a", "The dividend"};
    FunctionDocumentation::Argument argument2 = {"b", "The divisor (modulus)"};
    FunctionDocumentation::Arguments arguments = {argument1, argument2};
    FunctionDocumentation::ReturnedValue returned_value = {"The remainder of a % b"};
    FunctionDocumentation::Example example1 = {"Usage example", "SELECT modulo(5, 2)", "1"};
    FunctionDocumentation::Examples examples = {example1};
    FunctionDocumentation::IntroducedIn introduced_in = {1, 1};
    FunctionDocumentation::Category categories = FunctionDocumentation::Category::Arithmetic;
    FunctionDocumentation documentation = {description, syntax, arguments, returned_value, examples, introduced_in, categories};
    factory.registerFunction<FunctionModulo>(documentation);
    factory.registerAlias("mod", "modulo", FunctionFactory::Case::Insensitive);
}

struct NameModuloLegacy { static constexpr auto name = "moduloLegacy"; };
using FunctionModuloLegacy = BinaryArithmeticOverloadResolver<ModuloLegacyImpl, NameModuloLegacy, false>;

REGISTER_FUNCTION(ModuloLegacy)
{
    factory.registerFunction<FunctionModuloLegacy>();
}

struct NamePositiveModulo
{
    static constexpr auto name = "positiveModulo";
};
using FunctionPositiveModulo = BinaryArithmeticOverloadResolver<PositiveModuloImpl, NamePositiveModulo, false>;

REGISTER_FUNCTION(PositiveModulo)
{
    FunctionDocumentation::Description description = R"(
Calculates the remainder when dividing `x` by `y`. Similar to function
`modulo` except that `positiveModulo` always return non-negative number.
    )";
    FunctionDocumentation::Syntax syntax = "positiveModulo(x, y)";
    FunctionDocumentation::Arguments arguments = {
        {"x", "The dividend.", {"(U)Int*", "Float*", "Decimal"}},
        {"y", "The divisor (modulus).", {"(U)Int*", "Float*", "Decimal"}}
    };
    FunctionDocumentation::ReturnedValue returned_value = {R"(
Returns the difference between `x` and the nearest integer not greater than
`x` divisible by `y`.
    )"};
    FunctionDocumentation::Examples example = {{"Usage example", "SELECT positiveModulo(-1, 10)", "9"}};
    FunctionDocumentation::IntroducedIn introduced_in = {22, 11};
    FunctionDocumentation::Category categories = FunctionDocumentation::Category::Arithmetic;
    FunctionDocumentation documentation = {description, syntax, arguments, returned_value, example, introduced_in, categories};

    factory.registerFunction<FunctionPositiveModulo>(documentation,
        FunctionFactory::Case::Insensitive);

    factory.registerAlias("positive_modulo", "positiveModulo", FunctionFactory::Case::Insensitive);
    /// Compatibility with Spark:
    factory.registerAlias("pmod", "positiveModulo", FunctionFactory::Case::Insensitive);
}

}
