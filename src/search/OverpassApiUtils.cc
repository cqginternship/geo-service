#include "OverpassApiUtils.h"

#include "../utils/JsonUtils.h"
#include "../utils/WebClient.h"
#include "ProtoTypes.h"

#include <rapidjson/document.h>
#include <format>

namespace
{

using namespace geo;

constexpr const char* sz_requestByNameFormat =
   "[out:json];"
   "rel[\"name\"=\"{0}\"][\"boundary\"=\"administrative\"];"
   "out ids;";

constexpr const char* sz_requestByCoordinatesFormat =
   "[out:json];"
   "is_in({},{}) -> .areas;"
   "("
   "rel(pivot.areas)[\"boundary\"=\"administrative\"];"
   "rel(pivot.areas)[\"place\"~\"^(city|town|state)$\"];"
   ");"
   "out ids;";

constexpr const char* sz_requestTourismNodesFormat =
   "[out:json];"
   "rel({0});"
   "node(r)[\"tourism\"];"
   "out body;";

}  // namespace

namespace geo::overpass
{

OsmIds ExtractRelationIds(const std::string& json)
{
   if (json.empty())
      return {};

   rapidjson::Document document;
   document.Parse(json.c_str());
   if (!document.IsObject())
      return {};

   OsmIds result;
   for (const auto& e : document["elements"].GetArray())
   {
      const auto& id = json::Get(e, "id");
      if (!id.IsNull() && json::GetString(json::Get(e, "type")) == "relation")
         result.emplace_back(json::GetInt64(id));
   }
   return result;
}

OsmNodes ExtractNodes(const std::string& json)
{
   OsmNodes nodes;
   if (json.empty())
      return nodes;

   rapidjson::Document document;
   document.Parse(json.c_str());
   if (!document.IsObject())
      return nodes;

   for (const auto& e : document["elements"].GetArray())
   {
      if (json::GetString(json::Get(e, "type")) != "node")
         continue;

      OsmNode node;
      node.lat = json::GetDouble(json::Get(e, "lat"));
      node.lon = json::GetDouble(json::Get(e, "lon"));

      const auto& tagsObj = json::Get(e, "tags");
      if (tagsObj.IsObject())
      {
         for (auto it = tagsObj.MemberBegin(); it != tagsObj.MemberEnd(); ++it)
         {
            node.tags[it->name.GetString()] = it->value.GetString();
         }
      }

      nodes.emplace_back(std::move(node));
   }

   return nodes;
}

OsmIds LoadRelationIdsByName(WebClient& client, const std::string& name)
{
   const std::string request = std::format(sz_requestByNameFormat, name);
   const std::string response = client.Post(request);
   return ExtractRelationIds(response);
}

OsmIds LoadRelationIdsByLocation(WebClient& client, double latitude, double longitude)
{
   const std::string request = std::format(sz_requestByCoordinatesFormat, latitude, longitude);
   const std::string response = client.Post(request);
   return ExtractRelationIds(response);
}

OsmNodes LoadTourismNodesForRelation(WebClient& client, OsmId relationId)
{
   const std::string request = std::format(sz_requestTourismNodesFormat, relationId);
   const std::string response = client.Post(request);
   return ExtractNodes(response);
}

}  // namespace geo::overpass
