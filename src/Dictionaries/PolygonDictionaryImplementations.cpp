#include <Dictionaries/PolygonDictionaryImplementations.h>
#include <Dictionaries/DictionaryFactory.h>

#include <DataTypes/DataTypeArray.h>
#include <DataTypes/DataTypeTuple.h>
#include <DataTypes/DataTypesNumber.h>
#include <Dictionaries/ClickHouseDictionarySource.h>
#include <Dictionaries/DictionarySourceHelpers.h>

#include <Interpreters/Context.h>
#include <Common/logger_useful.h>
#include <Core/Settings.h>

namespace DB
{
namespace Setting
{
    extern const SettingsBool dictionary_use_async_executor;
}

namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
}

PolygonDictionarySimple::PolygonDictionarySimple(
        const StorageID & dict_id_,
        const DictionaryStructure & dict_struct_,
        DictionarySourcePtr source_ptr_,
        const DictionaryLifetime dict_lifetime_,
        Configuration configuration_)
    : IPolygonDictionary(dict_id_, dict_struct_, std::move(source_ptr_), dict_lifetime_, configuration_)
{
}

std::shared_ptr<IExternalLoadable> PolygonDictionarySimple::clone() const
{
    return std::make_shared<PolygonDictionarySimple>(
            this->getDictionaryID(),
            this->dict_struct,
            this->source_ptr->clone(),
            this->dict_lifetime,
            this->configuration);
}

bool PolygonDictionarySimple::find(const Point & point, size_t & polygon_index) const
{
    bool found = false;
    for (size_t i = 0; i < polygons.size(); ++i)
    {
        if (bg::covered_by(point, polygons[i]))
        {
            polygon_index = i;
            found = true;
            break;
        }
    }
    return found;
}

PolygonDictionaryIndexEach::PolygonDictionaryIndexEach(
        const StorageID & dict_id_,
        const DictionaryStructure & dict_struct_,
        DictionarySourcePtr source_ptr_,
        const DictionaryLifetime dict_lifetime_,
        Configuration configuration_,
        int min_intersections_,
        int max_depth_)
        : IPolygonDictionary(dict_id_, dict_struct_, std::move(source_ptr_), dict_lifetime_, configuration_),
          grid(min_intersections_, max_depth_, polygons),
          min_intersections(min_intersections_),
          max_depth(max_depth_)
{
    buckets.reserve(polygons.size());
    for (const auto & polygon : polygons)
    {
        std::vector<Polygon> single;
        single.emplace_back(polygon);
        buckets.emplace_back(single);
    }
}

std::shared_ptr<IExternalLoadable> PolygonDictionaryIndexEach::clone() const
{
    return std::make_shared<PolygonDictionaryIndexEach>(
            this->getDictionaryID(),
            this->dict_struct,
            this->source_ptr->clone(),
            this->dict_lifetime,
            this->configuration,
            this->min_intersections,
            this->max_depth);
}

bool PolygonDictionaryIndexEach::find(const Point & point, size_t & polygon_index) const
{
    const auto * cell = grid.find(point.x(), point.y());
    if (cell)
    {
        for (const auto & candidate : cell->polygon_ids)
        {
            size_t unused;
            if (buckets[candidate].find(point, unused))
            {
                polygon_index = candidate;
                return true;
            }
        }
        if (cell->first_covered != FinalCell::kNone)
        {
            polygon_index = cell->first_covered;
            return true;
        }
    }
    return false;
}

PolygonDictionaryIndexCell::PolygonDictionaryIndexCell(
    const StorageID & dict_id_,
    const DictionaryStructure & dict_struct_,
    DictionarySourcePtr source_ptr_,
    const DictionaryLifetime dict_lifetime_,
    Configuration configuration_,
    size_t min_intersections_,
    size_t max_depth_)
    : IPolygonDictionary(dict_id_, dict_struct_, std::move(source_ptr_), dict_lifetime_, configuration_),
      index(min_intersections_, max_depth_, polygons),
      min_intersections(min_intersections_),
      max_depth(max_depth_)
{
}

std::shared_ptr<IExternalLoadable> PolygonDictionaryIndexCell::clone() const
{
    return std::make_shared<PolygonDictionaryIndexCell>(
            this->getDictionaryID(),
            this->dict_struct,
            this->source_ptr->clone(),
            this->dict_lifetime,
            this->configuration,
            this->min_intersections,
            this->max_depth);
}

bool PolygonDictionaryIndexCell::find(const Point & point, size_t & polygon_index) const
{
    const auto * cell = index.find(point.x(), point.y());
    if (cell)
    {
        if (!(cell->corresponding_ids).empty() && cell->index.find(point, polygon_index))
        {
            polygon_index = cell->corresponding_ids[polygon_index];
            return true;
        }
        if (cell->first_covered != FinalCellWithSlabs::kNone)
        {
            polygon_index = cell->first_covered;
            return true;
        }
    }
    return false;
}

template <class PolygonDictionary>
DictionaryPtr createLayout(const std::string & /*name*/,
                           const DictionaryStructure & dict_struct,
                           const Poco::Util::AbstractConfiguration & config,
                           const std::string & config_prefix,
                           DictionarySourcePtr source_ptr,
                           ContextPtr global_context,
                           bool /*created_from_ddl*/)
{
    const String name = config.getString(config_prefix + ".name");

    if (!dict_struct.key)
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "'key' is required for a polygon dictionary");
    if (dict_struct.key->size() != 1)
        throw Exception(ErrorCodes::BAD_ARGUMENTS,
            "The 'key' should consist of a single attribute for a polygon dictionary");

    const auto key_type = (*dict_struct.key)[0].type;
    const auto f64 = std::make_shared<DataTypeFloat64>();
    const auto multi_polygon_array = DataTypeArray(std::make_shared<DataTypeArray>(std::make_shared<DataTypeArray>(std::make_shared<DataTypeArray>(f64))));
    const auto multi_polygon_tuple = DataTypeArray(std::make_shared<DataTypeArray>(std::make_shared<DataTypeArray>(std::make_shared<DataTypeTuple>(std::vector<DataTypePtr>{f64, f64}))));
    const auto simple_polygon_array = DataTypeArray(std::make_shared<DataTypeArray>(f64));
    const auto simple_polygon_tuple = DataTypeArray(std::make_shared<DataTypeTuple>(std::vector<DataTypePtr>{f64, f64}));

    IPolygonDictionary::InputType input_type;
    IPolygonDictionary::PointType point_type;

    if (key_type->equals(multi_polygon_array))
    {
        input_type = IPolygonDictionary::InputType::MultiPolygon;
        point_type = IPolygonDictionary::PointType::Array;
    }
    else if (key_type->equals(multi_polygon_tuple))
    {
        input_type = IPolygonDictionary::InputType::MultiPolygon;
        point_type = IPolygonDictionary::PointType::Tuple;
    }
    else if (key_type->equals(simple_polygon_array))
    {
        input_type = IPolygonDictionary::InputType::SimplePolygon;
        point_type = IPolygonDictionary::PointType::Array;
    }
    else if (key_type->equals(simple_polygon_tuple))
    {
        input_type = IPolygonDictionary::InputType::SimplePolygon;
        point_type = IPolygonDictionary::PointType::Tuple;
    }
    else
    {
        throw Exception(ErrorCodes::BAD_ARGUMENTS,
            "The key type {} is not one of the following allowed types for a polygon dictionary: {} {} {} {} ",
            key_type->getName(),
            multi_polygon_array.getName(),
            multi_polygon_tuple.getName(),
            simple_polygon_array.getName(),
            simple_polygon_tuple.getName());
    }

    const auto & layout_prefix = config_prefix + ".layout";
    Poco::Util::AbstractConfiguration::Keys keys;
    config.keys(layout_prefix, keys);
    const auto & dict_prefix = layout_prefix + "." + keys.front();

    ContextMutablePtr context = copyContextAndApplySettingsFromDictionaryConfig(global_context, config, config_prefix);
    const auto * clickhouse_source = dynamic_cast<const ClickHouseDictionarySource *>(source_ptr.get());
    bool use_async_executor = clickhouse_source && clickhouse_source->isLocal() && context->getSettingsRef()[Setting::dictionary_use_async_executor];

    IPolygonDictionary::Configuration configuration
    {
        .input_type = input_type,
        .point_type = point_type,
        .store_polygon_key_column = config.getBool(dict_prefix + ".store_polygon_key_column", false),
        .use_async_executor = use_async_executor,
    };

    if (dict_struct.range_min || dict_struct.range_max)
        throw Exception(ErrorCodes::BAD_ARGUMENTS,
            "{}: elements range_min and range_max should be defined only "
            "for a dictionary of layout 'range_hashed'",
            name);

    const DictionaryLifetime dict_lifetime{config, config_prefix + ".lifetime"};

    const auto dict_id = StorageID::fromDictionaryConfig(config, config_prefix);

    if constexpr (std::is_same_v<PolygonDictionary, PolygonDictionaryIndexEach> || std::is_same_v<PolygonDictionary, PolygonDictionaryIndexCell>)
    {
        size_t max_depth = config.getUInt(dict_prefix + ".max_depth", PolygonDictionary::kMaxDepthDefault);
        size_t min_intersections = config.getUInt(dict_prefix + ".min_intersections", PolygonDictionary::kMinIntersectionsDefault);
        return std::make_unique<PolygonDictionary>(dict_id, dict_struct, std::move(source_ptr), dict_lifetime, configuration, min_intersections, max_depth);
    }
    else
        return std::make_unique<PolygonDictionary>(dict_id, dict_struct, std::move(source_ptr), dict_lifetime, configuration);
}

void registerDictionaryPolygon(DictionaryFactory & factory)
{
    factory.registerLayout("polygon_simple", createLayout<PolygonDictionarySimple>, true);
    factory.registerLayout("polygon_index_each", createLayout<PolygonDictionaryIndexEach>, true);
    factory.registerLayout("polygon_index_cell", createLayout<PolygonDictionaryIndexCell>, true);

    /// Alias to the most performant dictionary type - polygon_index_cell
    factory.registerLayout("polygon", createLayout<PolygonDictionaryIndexCell>, true);
}

}
