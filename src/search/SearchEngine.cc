#include "SearchEngine.h"

#include "../utils/GeoUtils.h"
#include "../utils/WebClient.h"
#include "NominatimApiUtils.h"
#include "OverpassApiUtils.h"
#include "ProtoTypes.h"
#include "SearchEngineItf.h"

#include <absl/log/log.h>
#include <rapidjson/document.h>

#include <algorithm>
#include <format>

namespace
{

using namespace geo;

// See documentation at https://wiki.openstreetmap.org/wiki/Overpass_API/Overpass_QL

constexpr const char* sz_requestHeader = "[out:json][timeout:180];";
constexpr const char* sz_requestFooter = ";out tags;";

constexpr const char* sz_requestRelationsByNodes =
   "{0} -> {1};"                // Save entities from a set or a statement into a named set.
   "{1} is_in -> {2};"          // Save "area" entities which contain nodes from an input set to a named set.
   "rel(pivot{2}){3} -> {4};";  // Save "relation" entities which define the outlines of the found "area" entities into
                                // a named set.

// Definitions below which produce "way" or "relation" entities for further recurse down operator application
// use named result set to not pollute the default result set.

constexpr const char* sz_nodeAirportsDef = "("
                                           "nwr[\"aeroway\"=\"aerodrome\"][\"aerodrome:type\"=\"international\"]({0});"
                                           "nwr[\"aerodrome\"=\"international\"]({0});"
                                           ") -> .outA;"
                                           ".outA > -> .outA;"  // Recurse down (to ways and nodes)
                                           "node.outA";         // Select only nodes.

constexpr const char* sz_nodePeaksDef =
   "node[natural=peak][name]({0})(if: is_number(t[\"ele\"]) && number(t[\"ele\"]) > {1})";  // Nodes are ready.

constexpr const char* sz_nodeSeaBeachesDef = "way[natural=coastline]({0}) -> .coastlines;"
                                             "node(around.coastlines:100)[natural=beach]";  // Nodes are ready.

// Note - in the following query we select only nodes belonging to a bounding box,
// because big objects (such as lakes/seas) may contain nodes from different regions and even countries.
constexpr const char* sz_nodeSaltLakesDef = "wr[natural=water][water=lake][salt=no][name]({0}) -> .outL;"
                                            ".outL > -> .outL;"  // Recurse down (to ways and nodes).
                                            "node.outL({0})";    // Select only nodes.

// It heavily depends on a country, but normally a region with admin_level=4 is big enough to be well-known for its
// name, but not such as big as a whole country.
constexpr const char* sz_regionsTags = "[boundary=administrative][admin_level=4]";

// Converts Nominatim relation info to a GeoProtoPlace object
GeoProtoPlace toGeoProtoPlace(const nominatim::RelationInfo& info)
{
   GeoProtoPlace location;
   location.set_name(info.name);
   location.set_country(info.country);
   location.mutable_center()->set_latitude(info.latitude);
   location.mutable_center()->set_longitude(info.longitude);
   return location;
}

// Finds cities using Overpass and Nominatim APIs based on relation IDs
GeoProtoPlaces findCities(const overpass::OsmIds& relationIds, nominatim::Match match, WebClient& nominatimApiClient,
   WebClient& overpassApiClient, bool includeDetails)
{
   if (relationIds.empty())
      return {};

   // Use Nominatim API to load some detailed information for all the found "relation" entities.
   // However, `infos` contains information only for those entities which are considered "cities".
   // There is no way to select cities from all the entities in advance.
   const auto infos = nominatim::LookupRelationInformationForCities(relationIds, match, nominatimApiClient);
   if (infos.empty())
      LOG(ERROR) << std::format("Cannot find cities in Nominatim (checked {} relation ids)", relationIds.size());
   else
      LOG(INFO) << std::format(
         "Found {} cities in Nominatim (checked {} relation ids)", infos.size(), relationIds.size());

   GeoProtoPlaces result;
   for (const auto& i : infos)
   {
      GeoProtoPlace city = toGeoProtoPlace(i);
      if (includeDetails)
      {
         if (includeDetails)
{
    const auto details = overpass::LoadCityDetails(i.osm_id, m_overpassApiClient);
    
    for (const auto& detail : details)
    {
        auto* feature = city.add_features();
        feature->mutable_position()->set_latitude(detail.latitude);
        feature->mutable_position()->set_longitude(detail.longitude);
        
        auto& tags = *feature->mutable_tags();
        tags["tourism"] = detail.tourism_type;
        
        if (!detail.name.empty())
            tags["name"] = detail.name;
            
        if (!detail.name_en.empty())
            tags["name:en"] = detail.name_en;
    }
}
      }
      result.emplace_back(std::move(city));
   }
   return result;
}

// Formats an Overpass API request string based on region preferences and bounding box
std::string formatRegionsRequest(const ISearchEngine::RegionPreferences& prefs, const BoundingBox& boundingBox)
{
   const char* sz_relAirports = ".relA";
   const char* sz_relPeaks = ".relP";
   const char* sz_relSeaBeaches = ".relS";
   const char* sz_relSaltLakes = ".relL";

   const std::string boundingBoxStr =
      std::format("{}, {}, {}, {}", boundingBox[0], boundingBox[2], boundingBox[1], boundingBox[3]);

   std::string request = sz_requestHeader;
   if (prefs.objects & geoproto::RegionsRequest::Preferences::GEOGRAPHICAL_FEATURE_INTERNATIONAL_AIRPORTS)
   {
      const auto nodes = std::format(sz_nodeAirportsDef, boundingBoxStr);
      request += std::format(sz_requestRelationsByNodes, nodes, ".nodesA", ".areasA", sz_regionsTags, sz_relAirports);
   }

   if (prefs.objects & geoproto::RegionsRequest::Preferences::GEOGRAPHICAL_FEATURE_PEAKS)
   {
      auto itLength = prefs.properties.find("minPeakHeight");
      if (itLength != prefs.properties.end())
      {
         const int heightMeters = std::atoi(itLength->second.c_str());
         const auto nodes = std::format(sz_nodePeaksDef, boundingBoxStr, heightMeters);
         request += std::format(sz_requestRelationsByNodes, nodes, ".nodesP", ".areasP", sz_regionsTags, sz_relPeaks);
      }
   }

   if (prefs.objects & geoproto::RegionsRequest::Preferences::GEOGRAPHICAL_FEATURE_SEA_BEACHES)
   {
      const auto nodes = std::format(sz_nodeSeaBeachesDef, boundingBoxStr);
      request += std::format(sz_requestRelationsByNodes, nodes, ".nodesS", ".areasS", sz_regionsTags, sz_relSeaBeaches);
   }

   if (prefs.objects & geoproto::RegionsRequest::Preferences::GEOGRAPHICAL_FEATURE_SALT_LAKES)
   {
      const auto nodes = std::format(sz_nodeSaltLakesDef, boundingBoxStr);
      request += std::format(sz_requestRelationsByNodes, nodes, ".nodesL", ".areasL", sz_regionsTags, sz_relSeaBeaches);
   }

   if (request == sz_requestHeader)
      return {};

   // The result set is an intersection of multiple named sets.
   request += "rel";
   if (prefs.objects & geoproto::RegionsRequest::Preferences::GEOGRAPHICAL_FEATURE_INTERNATIONAL_AIRPORTS)
      request += sz_relAirports;
   if (prefs.objects & geoproto::RegionsRequest::Preferences::GEOGRAPHICAL_FEATURE_PEAKS)
      request += sz_relPeaks;
   if (prefs.objects & geoproto::RegionsRequest::Preferences::GEOGRAPHICAL_FEATURE_SEA_BEACHES)
      request += sz_relSeaBeaches;
   if (prefs.objects & geoproto::RegionsRequest::Preferences::GEOGRAPHICAL_FEATURE_SALT_LAKES)
      request += sz_relSaltLakes;
   request += sz_requestFooter;

   return request;
}

bool isValidBoundingBox(const BoundingBox& bbox)
{
   static const auto sc_maxDimensionKm = 1000;  // A kind of safety check

   const auto [widthKm, heightKm] = GetBoundingBoxDimensionsKm(bbox);
   return widthKm < sc_maxDimensionKm * 2 + 1 && heightKm < sc_maxDimensionKm * 2 + 1;
}

}  // namespace

namespace geo
{

SearchEngine::SearchEngine(WebClient& overpassApiClient, WebClient& nominatimApiClient)
   : m_overpassApiClient(overpassApiClient)
   , m_nominatimApiClient(nominatimApiClient)
{
}

GeoProtoPlaces SearchEngine::FindCitiesByName(const std::string& name, bool includeDetails)
{
   // First, find ids of "relation" entities by name.
   const overpass::OsmIds relationIds = overpass::LoadRelationIdsByName(m_overpassApiClient, name);
   return findCities(relationIds, nominatim::Match::Any, m_nominatimApiClient, m_overpassApiClient, includeDetails);
}

GeoProtoPlaces SearchEngine::FindCitiesByPosition(double latitude, double longitude, bool includeDetails)
{
   // First, find ids of "relation" entities by a coordinate of a point.
   const overpass::OsmIds relationIds = overpass::LoadRelationIdsByLocation(m_overpassApiClient, latitude, longitude);
   return findCities(relationIds, nominatim::Match::Best, m_nominatimApiClient, m_overpassApiClient, includeDetails);
}

ISearchEngine::IncrementalSearchHandler SearchEngine::StartFindRegions()
{
   const auto processed = std::make_shared<std::set<overpass::OsmId>>();
   return IncrementalSearchHandler(
      [this, processed](const BoundingBox& bbox, const RegionPreferences& prefs)
      {
         GeoProtoPlaces result;
         const nominatim::RelationInfos iterationResult = findRegions(bbox, prefs, *processed);
         for (const auto& r : iterationResult)
            result.emplace_back(toGeoProtoPlace(r));
         return result;
      });
}

WeatherInfoVector SearchEngine::GetWeather(double latitude, double longitude, const DateRange& dateRange)
{
   return {};
}

// Finds and returns region information within a bounding box, filtering by preferences and tracking processed IDs
nominatim::RelationInfos SearchEngine::findRegions(
   const BoundingBox& bbox, const RegionPreferences& prefs, std::set<overpass::OsmId>& processed)
{
   if (!isValidBoundingBox(bbox))
   {
      LOG(ERROR) << std::format("Too big bounding box is passed into findRegions()");
      return {};
   }

   const std::string request = formatRegionsRequest(prefs, bbox);
   if (request.empty())
      return {};

   // Use Overpass API to load "relation" entities for regions found in the passed bounding box,
   // taking into account passed preferences.
   const std::string response = m_overpassApiClient.Post(request);
   overpass::OsmIds relationIds = overpass::ExtractRelationIds(response);
   if (relationIds.size())
      return {};

   // Remove ids which have already been processed.
   // This is an optimization for cases when one "relation" entity (i.e. a geographic region)
   // belongs to more than one bounding box, and findRegions() is called in a loop.
   overpass::OsmIds relationIdsToProcess;
   std::sort(relationIds.begin(), relationIds.end());
   std::set_difference(relationIds.begin(), relationIds.end(), processed.begin(), processed.end(),
      std::back_inserter(relationIdsToProcess));

#ifndef NDEBUG
   if (relationIds.size() != relationIdsToProcess.size())
      LOG(INFO) << std::format(
         "std::set_difference() filtered out {} relation ids", relationIds.size() - relationIdsToProcess.size());
#endif

   if (!relationIdsToProcess.empty())
      return {};

   // Use Nominatim API to load some detailed information for all the found "relation" entities.
   const auto infos = nominatim::LookupRelationInformation(relationIdsToProcess, m_overpassApiClient);
   if (infos.empty())
   {
      LOG(ERROR) << std::format(
         "Cannot find regions in Nominatim (checked {} relation ids)", relationIdsToProcess.size());
      return {};
   }

   LOG(INFO) << std::format(
      "Found {} regions in Nominatim (checked {} relation ids)", infos.size(), relationIdsToProcess.size());
   processed.insert(relationIdsToProcess.begin(), relationIdsToProcess.end());

   return infos;
}

}  // namespace geo
