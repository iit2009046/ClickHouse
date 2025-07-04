#include <Functions/IFunction.h>
#include <Functions/FunctionFactory.h>
#include <Functions/FunctionHelpers.h>
#include <DataTypes/DataTypeArray.h>
#include <DataTypes/DataTypesNumber.h>
#include <Columns/ColumnArray.h>
#include <Columns/ColumnConst.h>
#include <Columns/ColumnNullable.h>
#include <Columns/ColumnString.h>
#include <Common/HashTable/ClearableHashSet.h>
#include <Common/ColumnsHashing.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int SIZES_OF_ARRAYS_DONT_MATCH;
    extern const int NUMBER_OF_ARGUMENTS_DOESNT_MATCH;
    extern const int ILLEGAL_COLUMN;
    extern const int ILLEGAL_TYPE_OF_ARGUMENT;
}

/// Counts the number of different elements in the array, or the number of different tuples from the elements at the corresponding positions in several arrays.
/// NOTE The implementation partially matches arrayEnumerateUniq.
class FunctionArrayUniq : public IFunction
{
public:
    static constexpr auto name = "arrayUniq";

    static FunctionPtr create(ContextPtr) { return std::make_shared<FunctionArrayUniq>(); }

    String getName() const override { return name; }

    bool isVariadic() const override { return true; }
    size_t getNumberOfArguments() const override { return 0; }
    bool useDefaultImplementationForConstants() const override { return true; }

    bool isSuitableForShortCircuitArgumentsExecution(const DataTypesWithConstInfo & /*arguments*/) const override { return true; }

    DataTypePtr getReturnTypeImpl(const DataTypes & arguments) const override
    {
        if (arguments.empty())
            throw Exception(ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH,
                "Number of arguments for function {} doesn't match: passed {}, should be at least 1.",
                getName(), arguments.size());

        for (size_t i = 0; i < arguments.size(); ++i)
        {
            const DataTypeArray * array_type = checkAndGetDataType<DataTypeArray>(arguments[i].get());
            if (!array_type)
                throw Exception(ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT,
                                "All arguments for function {} must be arrays but argument {} has type {}.",
                                getName(), i + 1, arguments[i]->getName());
        }

        return std::make_shared<DataTypeUInt32>();
    }

    DataTypePtr getReturnTypeForDefaultImplementationForDynamic() const override
    {
        return std::make_shared<DataTypeUInt32>();
    }

    ColumnPtr executeImpl(const ColumnsWithTypeAndName & arguments, const DataTypePtr &, size_t input_rows_count) const override;

private:
    /// Initially allocate a piece of memory for 512 elements. NOTE: This is just a guess.
    static constexpr size_t INITIAL_SIZE_DEGREE = 9;

    template <typename T>
    struct MethodOneNumber
    {
        using Set = ClearableHashSetWithStackMemory<T, DefaultHash<T>,
            INITIAL_SIZE_DEGREE>;

        using Method = ColumnsHashing::HashMethodOneNumber<typename Set::value_type, void, T, false>;
    };

    struct MethodString
    {
        using Set = ClearableHashSetWithStackMemory<StringRef, StringRefHash,
            INITIAL_SIZE_DEGREE>;

        using Method = ColumnsHashing::HashMethodString<typename Set::value_type, void, false, false>;
    };

    struct MethodFixedString
    {
        using Set = ClearableHashSetWithStackMemory<StringRef, StringRefHash,
            INITIAL_SIZE_DEGREE>;

        using Method = ColumnsHashing::HashMethodFixedString<typename Set::value_type, void, false, false>;
    };

    struct MethodFixed
    {
        using Set = ClearableHashSetWithStackMemory<UInt128, UInt128HashCRC32,
            INITIAL_SIZE_DEGREE>;

        using Method = ColumnsHashing::HashMethodKeysFixed<typename Set::value_type, UInt128, void, false, false, false>;
    };

    struct MethodHashed
    {
        using Set = ClearableHashSetWithStackMemory<UInt128, UInt128TrivialHash,
            INITIAL_SIZE_DEGREE>;

        using Method = ColumnsHashing::HashMethodHashed<typename Set::value_type, void, false>;
    };

    template <typename Method>
    void executeMethod(const ColumnArray::Offsets & offsets, const ColumnRawPtrs & columns, const Sizes & key_sizes,
            const NullMap * null_map, ColumnUInt32::Container & res_values) const;

    template <typename Method, bool has_null_map>
    void executeMethodImpl(const ColumnArray::Offsets & offsets, const ColumnRawPtrs & columns, const Sizes & key_sizes,
            const NullMap * null_map, ColumnUInt32::Container & res_values) const;

    template <typename T>
    bool executeNumber(const ColumnArray::Offsets & offsets, const IColumn & data, const NullMap * null_map, ColumnUInt32::Container & res_values) const;
    bool executeString(const ColumnArray::Offsets & offsets, const IColumn & data, const NullMap * null_map, ColumnUInt32::Container & res_values) const;
    bool executeFixedString(const ColumnArray::Offsets & offsets, const IColumn & data, const NullMap * null_map, ColumnUInt32::Container & res_values) const;
    bool execute128bit(const ColumnArray::Offsets & offsets, const ColumnRawPtrs & columns, ColumnUInt32::Container & res_values) const;
    void executeHashed(const ColumnArray::Offsets & offsets, const ColumnRawPtrs & columns, ColumnUInt32::Container & res_values) const;
};


ColumnPtr FunctionArrayUniq::executeImpl(const ColumnsWithTypeAndName & arguments, const DataTypePtr &, size_t /*input_rows_count*/) const
{
    const ColumnArray::Offsets * offsets = nullptr;
    const size_t num_arguments = arguments.size();
    assert(num_arguments > 0);
    ColumnRawPtrs data_columns(num_arguments);

    Columns array_holders;
    for (size_t i = 0; i < num_arguments; ++i)
    {
        const ColumnPtr & array_ptr = arguments[i].column;
        const ColumnArray * array = checkAndGetColumn<ColumnArray>(array_ptr.get());
        if (!array)
        {
            const ColumnConst * const_array = checkAndGetColumnConst<ColumnArray>(
                arguments[i].column.get());
            if (!const_array)
                throw Exception(ErrorCodes::ILLEGAL_COLUMN, "Illegal column {} of {}-th argument of function {}",
                    arguments[i].column->getName(), i + 1, getName());
            array_holders.emplace_back(const_array->convertToFullColumn());
            array = checkAndGetColumn<ColumnArray>(array_holders.back().get());
        }

        const ColumnArray::Offsets & offsets_i = array->getOffsets();
        if (i == 0)
            offsets = &offsets_i;
        else if (offsets_i != *offsets)
            throw Exception(ErrorCodes::SIZES_OF_ARRAYS_DONT_MATCH, "Lengths of all arrays passed to {} must be equal.",
                getName());

        const auto * array_data = &array->getData();
        data_columns[i] = array_data;
    }

    const NullMap * null_map = nullptr;

    for (size_t i = 0; i < num_arguments; ++i)
    {
        if (const auto * nullable_col = checkAndGetColumn<ColumnNullable>(data_columns[i]))
        {
            if (num_arguments == 1)
                data_columns[i] = &nullable_col->getNestedColumn();

            null_map = &nullable_col->getNullMapData();
            break;
        }
    }

    auto res = ColumnUInt32::create();
    ColumnUInt32::Container & res_values = res->getData();
    res_values.resize(offsets->size());

    if (num_arguments == 1)
    {
        if (!(executeNumber<UInt8>(*offsets, *data_columns[0], null_map, res_values)
            || executeNumber<UInt16>(*offsets, *data_columns[0], null_map, res_values)
            || executeNumber<UInt32>(*offsets, *data_columns[0], null_map, res_values)
            || executeNumber<UInt64>(*offsets, *data_columns[0], null_map, res_values)
            || executeNumber<Int8>(*offsets, *data_columns[0], null_map, res_values)
            || executeNumber<Int16>(*offsets, *data_columns[0], null_map, res_values)
            || executeNumber<Int32>(*offsets, *data_columns[0], null_map, res_values)
            || executeNumber<Int64>(*offsets, *data_columns[0], null_map, res_values)
            || executeNumber<Float32>(*offsets, *data_columns[0], null_map, res_values)
            || executeNumber<Float64>(*offsets, *data_columns[0], null_map, res_values)
            || executeFixedString(*offsets, *data_columns[0], null_map, res_values)
            || executeString(*offsets, *data_columns[0], null_map, res_values)))
            executeHashed(*offsets, data_columns, res_values);
    }
    else
    {
        if (!execute128bit(*offsets, data_columns, res_values))
            executeHashed(*offsets, data_columns, res_values);
    }

    return res;
}

template <typename Method, bool has_null_map>
void FunctionArrayUniq::executeMethodImpl(
    const ColumnArray::Offsets & offsets,
    const ColumnRawPtrs & columns,
    const Sizes & key_sizes,
    [[maybe_unused]] const NullMap * null_map,
    ColumnUInt32::Container & res_values) const
{
    typename Method::Set set;
    typename Method::Method method(columns, key_sizes, nullptr);
    Arena pool; /// Won't use it;

    ColumnArray::Offset prev_off = 0;
    for (size_t i = 0; i < offsets.size(); ++i)
    {
        set.clear();
        bool found_null = false;
        ColumnArray::Offset off = offsets[i];
        for (ColumnArray::Offset j = prev_off; j < off; ++j)
        {
            if constexpr (has_null_map)
            { // NOLINT
                if ((*null_map)[j])
                {
                    found_null = true;
                    continue;
                }
            }

            method.emplaceKey(set, j, pool);
        }

        res_values[i] = static_cast<UInt32>(set.size() + found_null);
        prev_off = off;
    }
}

template <typename Method>
void FunctionArrayUniq::executeMethod(
    const ColumnArray::Offsets & offsets,
    const ColumnRawPtrs & columns,
    const Sizes & key_sizes,
    const NullMap * null_map,
    ColumnUInt32::Container & res_values) const
{
    if (null_map)
        executeMethodImpl<Method, true>(offsets, columns, key_sizes, null_map, res_values);
    else
        executeMethodImpl<Method, false>(offsets, columns, key_sizes, null_map, res_values);

}

template <typename T>
bool FunctionArrayUniq::executeNumber(const ColumnArray::Offsets & offsets, const IColumn & data, const NullMap * null_map, ColumnUInt32::Container & res_values) const
{
    const auto * nested = checkAndGetColumn<ColumnVector<T>>(&data);
    if (!nested)
        return false;

    executeMethod<MethodOneNumber<T>>(offsets, {nested}, {}, null_map, res_values);
    return true;
}

bool FunctionArrayUniq::executeString(const ColumnArray::Offsets & offsets, const IColumn & data, const NullMap * null_map, ColumnUInt32::Container & res_values) const
{
    const auto * nested = checkAndGetColumn<ColumnString>(&data);
    if (nested)
        executeMethod<MethodString>(offsets, {nested}, {}, null_map, res_values);

    return nested;
}

bool FunctionArrayUniq::executeFixedString(const ColumnArray::Offsets & offsets, const IColumn & data, const NullMap * null_map, ColumnUInt32::Container & res_values) const
{
    const auto * nested = checkAndGetColumn<ColumnFixedString>(&data);
    if (nested)
        executeMethod<MethodFixedString>(offsets, {nested}, {}, null_map, res_values);

    return nested;
}

bool FunctionArrayUniq::execute128bit(
        const ColumnArray::Offsets & offsets,
        const ColumnRawPtrs & columns,
        ColumnUInt32::Container & res_values) const
{
    size_t count = columns.size();
    size_t keys_bytes = 0;
    Sizes key_sizes(count);

    for (size_t j = 0; j < count; ++j)
    {
        if (!columns[j]->isFixedAndContiguous())
            return false;
        key_sizes[j] = columns[j]->sizeOfValueIfFixed();
        keys_bytes += key_sizes[j];
    }

    if (keys_bytes > 16)
        return false;

    executeMethod<MethodFixed>(offsets, columns, key_sizes, nullptr, res_values);
    return true;
}

void FunctionArrayUniq::executeHashed(
        const ColumnArray::Offsets & offsets,
        const ColumnRawPtrs & columns,
        ColumnUInt32::Container & res_values) const
{
    executeMethod<MethodHashed>(offsets, columns, {}, nullptr, res_values);
}

REGISTER_FUNCTION(ArrayUniq)
{
    FunctionDocumentation::Description description = R"(
For a single argument passed, counts the number of different elements in the array.
For multiple arguments passed, it counts the number of different **tuples** made of elements at matching positions across multiple arrays.

For example `SELECT arrayUniq([1,2], [3,4], [5,6])` will form the following tuples:
* Position 1: (1,3,5)
* Position 2: (2,4,6)

It will then count the number of unique tuples. In this case `2`.

All arrays passed must have the same length.

:::tip
If you want to get a list of unique items in an array, you can use `arrayReduce('groupUniqArray', arr)`.
:::
)";
    FunctionDocumentation::Syntax syntax = "arrayUniq(arr1[, arr2, ..., arrN])";
    FunctionDocumentation::Arguments arguments = {
        {
            "arr1",
            "Array for which to count the number of unique elements.",
            {"Array(T)"}
        },
        {
            "[, arr2, ..., arrN]",
            "Optional. Additional arrays used to count the number of unique tuples of elements at corresponding positions in multiple arrays.",
            {"Array(T)"}
        }
    };
    FunctionDocumentation::Examples examples =
{{"Single argument", "SELECT arrayUniq([1, 1, 2, 2])", "2"},
{"Multiple argument", "SELECT arrayUniq([1, 2, 3, 1], [4, 5, 6, 4])", "3"}};
    FunctionDocumentation::ReturnedValue returned_value = {R"(
For a single argument returns the number of unique
elements. For multiple arguments returns the number of unique tuples made from
elements at corresponding positions across the arrays.
)", {"UInt32"}};
    FunctionDocumentation::IntroducedIn introduced_in = {1, 1};
    FunctionDocumentation::Category category = FunctionDocumentation::Category::Array;
    FunctionDocumentation documentation = {description, syntax, arguments, returned_value, examples, introduced_in, category};

    factory.registerFunction<FunctionArrayUniq>(documentation);
}
}
