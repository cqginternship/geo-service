#include "OpenMeteoApiUtils.h"

#include "../utils/JsonUtils.h"
#include "../utils/WebClient.h"

#include <absl/log/log.h>

#include <iomanip>
#include <sstream>

namespace geo::openmeteo
{

namespace
{

// Formats an Open Meteo API request string based on request parameters.
std::string formatHistoricalWeatherRequest(
   double latitude, double longitude, const Date& startDate, const Date& endDate)
{
   const char* sz_latitudeParam = "latitude";
   const char* sz_longitudeParam = "longitude";
   const char* sz_startDateParam = "start_date";
   const char* sz_endDateParam = "end_date";
   const char* sz_commonParams = "daily=temperature_2m_max,temperature_2m_min";

   std::string request;
   request += std::format("{}={}", sz_latitudeParam, latitude);
   request += std::format("&{}={}", sz_longitudeParam, longitude);
   request += std::format("&{}={:%F}", sz_startDateParam, startDate);
   request += std::format("&{}={:%F}", sz_endDateParam, endDate);
   request += std::format("&{}", sz_commonParams);
   return request;
}

// Parse Open Meteo API response.
WeatherInfoVector parseWeatherResponse(const std::string& response)
{
   rapidjson::Document document;
   document.Parse(response.c_str());

   const auto& timeValues = json::Get(document, "daily", "time").GetArray();
   const auto& temperatureMaxValues = json::Get(document, "daily", "temperature_2m_max").GetArray();
   const auto& temperatureMinValues = json::Get(document, "daily", "temperature_2m_min").GetArray();

   const std::size_t numValues = timeValues.Size();
   if (temperatureMaxValues.Size() != numValues || temperatureMinValues.Size() != numValues)
   {
      LOG(ERROR) << "Historical Weather response is malformed";
      return WeatherInfoVector{};
   }

   WeatherInfoVector result;
   result.resize(numValues);

   for (std::size_t i = 0; i < numValues; ++i)
   {
      WeatherInfo& info = result[i];
      info.time = StringToDate(json::GetString(timeValues[i]));
      info.temperatureMax = json::GetDouble(temperatureMaxValues[i]);
      info.temperatureMin = json::GetDouble(temperatureMinValues[i]);
      info.temperatureAverage = (info.temperatureMax + info.temperatureMin) / 2.0f;
   }

   return result;
}

}  // namespace

std::vector<DateRange> CollectHistoricalRanges(
   const DateRange& dateRange, const TimePoint& latestTime, std::uint32_t numYears)
{
   const std::chrono::years oneYear{1};
   const Date latestDate = TimePointToDate(latestTime);

   // Start from current year
   auto [startDate, endDate] = dateRange;
   const auto numDays = static_cast<std::chrono::sys_days>(endDate) - static_cast<std::chrono::sys_days>(startDate);
   startDate = latestDate.year() / startDate.month() / startDate.day();
   endDate = static_cast<std::chrono::sys_days>(startDate) + numDays;

   // If current date is less or equal to end date, go back one year
   while (latestDate <= endDate)
   {
      startDate -= oneYear;
      endDate -= oneYear;
   }

   // Collect past ranges starting from the most recent one
   std::vector<DateRange> result;
   for (std::uint32_t i = 0; i < std::max(1u, numYears); ++i)
   {
      result.emplace_back(startDate, endDate);

      startDate -= oneYear;
      endDate -= oneYear;
   }
   return result;
}

WeatherInfoVector LoadHistoricalWeather(
   WebClient& client, double latitude, double longitude, const DateRange& dateRange)
{
   const std::string request = formatHistoricalWeatherRequest(latitude, longitude, dateRange.first, dateRange.second);
   const std::string response = client.Get(request);
   return !response.empty() ? parseWeatherResponse(response) : WeatherInfoVector{};
}
OsmNodes LoadTourismNodesForRelation(WebClient& client, OsmId relationId)
{
    const std::string query = std::format(
        "[out:json][timeout:60];"
        "rel({});"
        "map_to_area;"
        "("
        "  node(area)[\"tourism\"=\"hotel\"];"
        "  node(area)[\"tourism\"=\"museum\"];"
        ");"
        "out tags center;",
        relationId
    );

    const std::string response = client.Post(query);

    return ExtractNodes(response);
}
}  // namespace geo::openmeteo
