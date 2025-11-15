#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace geo
{
class WebClient;
}  // namespace geo

namespace geo::overpass
{

using OsmId = std::int64_t;
using OsmIds = std::vector<OsmId>;
struct OsmNode
{
    double lat = 0.0;
    double lon = 0.0;
    std::map<std::string, std::string> tags;
};

using OsmNodes = std::vector<OsmNode>;

OsmIds ExtractRelationIds(const std::string& json);

OsmNodes ExtractNodes(const std::string& json);

OsmIds LoadRelationIdsByName(WebClient& client, const std::string& name);

OsmIds LoadRelationIdsByLocation(WebClient& client, double latitude, double longitude);

OsmNodes LoadTourismNodesForRelation(WebClient& client, OsmId relationId);

}  // namespace geo::overpass
