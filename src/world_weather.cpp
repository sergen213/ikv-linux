// NONCANONICAL wave 27: server-authoritative seasons, weather and wind (the original game had no weather at all; its clock was
// only the thgf hour + environment record). gameplay.json "weather" configures it; without the block nothing is simulated and
// clock.weather is never sent, so clients draw no weather.
//
// Determinism: everything that is not history-dependent is a pure function of (weather.seed, absolute in-game minute):
// the season (in-game day / season_days), the weather kind of each period_minutes segment (weighted pick from the season's
// table), its intensity, the target temperature (season range × diurnal curve of the displayed clock hour) and the wind target.
// The absolute minute continues across restarts: weather.start_day pins it (tests, reproducible captures), otherwise it is the
// wall clock scaled by clock.rate, so a restarted server resumes the same season and segment instead of jumping back to day 0
// (the day clock itself is not persisted and restarts at start_hour). The sent values are smoothed state (linear ramps over
// transition_minutes, exponential temperature/wind direction) plus integrators (wetness, snow cover); construction warm-starts
// them over the previous 48 in-game hours so a restart lands near the state a continuously running server would have.
#include "ikv/world.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <stdexcept>
#include <string>

namespace ikv {
namespace {
constexpr std::array<const char*,4> season_names{"spring","summer","autumn","winter"};
constexpr std::array<const char*,6> kind_names{"clear","cloudy","rain","storm","snow","fog"};
enum Kind : int { clear, cloudy, rain, storm, snow, fog };
constexpr double tau = 6.283185307179586, warm_minutes = 48*60, warm_step = 10;
uint64_t mix(uint64_t x) { x += 0x9e3779b97f4a7c15ull; x = (x^(x>>30))*0xbf58476d1ce4e5b9ull; x = (x^(x>>27))*0x94d049bb133111ebull; return x^(x>>31); }
// Uniform [0,1) for (seed, purpose, n): the same inputs give the same value on every server and restart.
double unit(int64_t seed, uint64_t salt, int64_t n) { return double(mix(mix(uint64_t(seed)*0x100000001b3ull+salt)^uint64_t(n))>>11)/9007199254740992.0; }
template <std::size_t N> int index_of(const std::array<const char*,N>& names, const std::string& s) { for (std::size_t i = 0; i < N; ++i) if (s == names[i]) return int(i); return -1; }
[[noreturn]] void fail(const std::string& message) { throw std::invalid_argument("gameplay config: weather: "+message); }
double real(const Json& v, double lo, double hi, const std::string& what) { if (!v.is_number() || !(v.get<double>() >= lo && v.get<double>() <= hi)) fail(what+" outside allowed range"); return v.get<double>(); }
int64_t whole(const Json& v, int64_t lo, int64_t hi, const std::string& what) { if (!v.is_number_integer() || v.get<int64_t>() < lo || v.get<int64_t>() > hi) fail(what+" must be an integer in range"); return v.get<int64_t>(); }
double approach(double v, double target, double step) { return v < target ? std::min(target, v+step) : std::max(target, v-step); }
double round_to(double v, double scale) { return std::round(v*scale)/scale; } // shortest JSON: 0.3, not 0.30000000000000004
bool precipitating(int kind) { return kind == rain || kind == storm || kind == snow; }
}

void World::weather_validate() const {
    if (!config_.contains("weather")) return;
    const auto& w = config_.at("weather");
    if (!w.is_object()) fail("block must be an object");
    if (!config_.contains("clock")) fail("requires the clock block (weather time runs at clock.rate)");
    whole(w.at("seed"), 0, INT32_MAX, "seed"); whole(w.at("season_days"), 1, 365, "season_days");
    if (w.contains("start_day")) whole(w.at("start_day"), 0, 1000000, "start_day");
    whole(w.at("period_minutes"), 10, 1440, "period_minutes"); whole(w.at("transition_minutes"), 1, 720, "transition_minutes");
    const auto& kinds = w.at("kinds");
    if (!kinds.is_object() || kinds.size() != kind_names.size()) fail("kinds must define clear, cloudy, rain, storm, snow and fog");
    for (const auto* name : kind_names) {
        const auto& k = kinds.at(name); const std::string at = std::string("kinds.")+name;
        for (const char* key : {"precipitation","cloud","fog"}) real(k.at(key), 0, 1, at+"."+key);
        const auto& wind = k.at("wind"); if (!wind.is_array() || wind.size() != 2) fail(at+".wind is [min, max] m/s");
        if (real(wind[0], 0, 25, at+".wind") > real(wind[1], 0, 25, at+".wind")) fail(at+".wind is [min, max] m/s");
        if (precipitating(index_of(kind_names, name)) != (k.at("precipitation").get<double>() > 0)) fail(at+": rain, storm and snow (only) precipitate");
    }
    const auto& seasons = w.at("seasons");
    if (!seasons.is_object() || seasons.size() != season_names.size()) fail("seasons must define spring, summer, autumn and winter");
    for (const auto* name : season_names) {
        const auto& s = seasons.at(name); const std::string at = std::string("seasons.")+name;
        const auto& weights = s.at("weights"); if (!weights.is_object() || weights.empty()) fail(at+".weights required");
        int64_t total = 0;
        for (const auto& [kind,weight] : weights.items()) { if (index_of(kind_names, kind) < 0) fail(at+".weights: unknown kind "+kind); total += whole(weight, 0, 1000, at+".weights."+kind); }
        if (total <= 0) fail(at+".weights must not all be zero");
        const auto& t = s.at("temperature"); if (!t.is_array() || t.size() != 2) fail(at+".temperature is [night low, day high] °C");
        if (real(t[0], -40, 50, at+".temperature") > real(t[1], -40, 50, at+".temperature")) fail(at+".temperature is [night low, day high] °C");
        real(s.at("wind_scale"), 0, 2, at+".wind_scale");
    }
}

double World::weather_minute() const {
    return weather_origin_+tick_*config_.at("clock").at("rate").get<double>()/1200.0; // 72000 ticks × rate per in-game hour
}

int World::weather_season(double minute) const {
    if (weather_.forced_season >= 0) return weather_.forced_season;
    const auto day = int64_t(std::floor(minute/1440)), length = config_.at("weather").at("season_days").get<int64_t>();
    return int(((day/length)%4+4)%4);
}

World::WeatherTarget World::weather_target(double minute) const {
    const auto& w = config_.at("weather"); const auto seed = w.at("seed").get<int64_t>();
    const auto segment = int64_t(std::floor(minute/w.at("period_minutes").get<double>()));
    const int season = weather_season(minute); const auto& s = w.at("seasons").at(season_names[season]);
    // The kind of a segment comes from the season at the segment start, so a season change never flips a running segment.
    int kind = weather_.forced_kind;
    if (kind < 0) {
        const auto& weights = w.at("seasons").at(season_names[weather_season(double(segment)*w.at("period_minutes").get<double>())]).at("weights");
        int64_t total = 0; for (const auto& [k,v] : weights.items()) { (void)k; total += v.get<int64_t>(); }
        double pick = unit(seed, 1, segment)*double(total); kind = clear;
        for (const auto* name : kind_names) { if (!weights.contains(name)) continue; pick -= weights.at(name).get<double>(); if (pick < 0) { kind = index_of(kind_names, std::string(name)); break; } }
    }
    const auto& k = w.at("kinds").at(kind_names[kind]); const double intensity = .75+.25*unit(seed, 2, segment);
    WeatherTarget t{kind, k.at("precipitation").get<double>()*intensity, std::min(1.0, k.at("cloud").get<double>()*(.85+.15*intensity)), k.at("fog").get<double>()*intensity, 0, 0, 0};
    const auto& wind = k.at("wind");
    t.wind = std::min(25.0, (wind[0].get<double>()+(wind[1].get<double>()-wind[0].get<double>())*unit(seed, 4, segment))*s.at("wind_scale").get<double>());
    // Night low at 03:00, day high at 15:00 of the displayed clock hour; clouds and precipitation keep the day cooler.
    const auto& clock = config_.at("clock");
    const double hour = std::fmod(std::fmod(clock.at("start_hour").get<double>()+clock_offset_+(minute-weather_origin_)/60.0, 24.0)+24.0, 24.0);
    const double low = s.at("temperature")[0].get<double>(), high = s.at("temperature")[1].get<double>(), day = .5+.5*std::cos(tau*(hour-15)/24);
    t.temperature = low+(high-low)*day*(1-.35*t.cloud)+(unit(seed, 3, segment)*4-2)-2*t.precipitation;
    if (weather_.forced_kind == snow) t.temperature = std::min(t.temperature, -1.0); // forced snow brings its cold with it
    // Prevailing direction drifts slowly over three days; each segment veers it by up to ±45°.
    t.angle = unit(seed, 5, 0)*tau+std::sin(tau*minute/(1440*3.0)+unit(seed, 6, 0)*tau)*tau/6+(unit(seed, 7, segment)-.5)*tau/4;
    return t;
}

// Advances the smoothed state by `minutes` in-game minutes ending at `minute`.
void World::weather_step(double minute, double minutes) {
    if (!(minutes > 0)) return;
    auto& s = weather_; const auto t = weather_target(minute);
    const double transition = config_.at("weather").at("transition_minutes").get<double>(), ramp = minutes/transition;
    s.temperature += (t.temperature-s.temperature)*(1-std::exp(-minutes/60));
    s.wind_angle += std::remainder(t.angle-s.wind_angle, tau)*(1-std::exp(-minutes/(2*transition)));
    s.wind_angle = std::remainder(s.wind_angle, tau);
    // Rain or snow from the smoothed temperature: snow only below 2 °C (a picked snow segment) or below 0 °C (any precipitation).
    int kind = t.kind;
    if (precipitating(kind)) kind = s.temperature < 2 && (kind == snow || s.temperature < 0) ? snow : kind == snow ? rain : kind;
    s.precipitation = approach(s.precipitation, t.precipitation, ramp); s.cloud = approach(s.cloud, t.cloud, ramp); s.fog = approach(s.fog, t.fog, ramp);
    s.wind_speed = approach(s.wind_speed, t.wind, 25*ramp);
    // While precipitation fades out after a rain/snow segment the sent kind stays the precipitating one.
    if (precipitating(kind)) s.kind = s.shown = kind;
    else s.kind = s.precipitation > .05 && precipitating(s.shown) ? s.shown : kind;
    if (s.kind == snow && s.temperature >= 2) s.kind = s.shown = rain;
    // Wetness soaks in ~20 min of full rain and dries over hours (faster when warm, slower under cloud); snow builds up over
    // ~90 min of full snowfall and melts above 0 °C (10 °C: one full cover per hour), the melt wetting the ground.
    if (s.kind == rain || s.kind == storm) s.wetness += s.precipitation*minutes/20;
    else s.wetness -= minutes*(1+std::max(0.0, s.temperature)/10)*(1-.5*s.cloud)/240;
    if (s.kind == snow) s.snow += s.precipitation*minutes/90;
    if (s.temperature > 0) { const double melt = std::min(s.snow, minutes*s.temperature/600); s.snow -= melt; s.wetness += melt*.5; }
    s.wetness = std::clamp(s.wetness, 0.0, 1.0); s.snow = std::clamp(s.snow, 0.0, 1.0);
}

void World::weather_start() {
    if (!config_.contains("weather")) return;
    const auto& w = config_.at("weather"); const auto& clock = config_.at("clock"); const double rate = clock.at("rate").get<double>();
    // Absolute in-game minute at tick 0: pinned by start_day, else the wall clock at clock.rate (continuous across restarts).
    weather_origin_ = w.contains("start_day") ? w.at("start_day").get<double>()*1440+clock.at("start_hour").get<double>()*60
                                              : std::floor(double(std::time(nullptr))*rate/60.0);
    weather_ = {};
    const auto start = weather_target(weather_origin_-warm_minutes);
    weather_.temperature = start.temperature; weather_.wind_angle = start.angle; weather_.wind_speed = start.wind;
    for (double m = weather_origin_-warm_minutes+warm_step; m <= weather_origin_; m += warm_step) weather_step(m, warm_step);
}

void World::weather_tick() {
    if (!config_.contains("weather")) return;
    if (weather_.forced_until && tick_ >= weather_.forced_until) { weather_.forced_kind = weather_.forced_season = -1; weather_.forced_until = 0; }
    if (tick_ % 20 == 0) weather_step(weather_minute(), 20*config_.at("clock").at("rate").get<double>()/1200.0);
}

Json World::weather_json() const {
    const auto& s = weather_; const auto seed = config_.at("weather").at("seed").get<int64_t>();
    // Gusts: smooth value noise over 2.5 s knots (real time), stronger as the wind rises.
    const int64_t knot = tick_/50; const double f = double(tick_%50)/50, e = f*f*(3-2*f);
    const double noise = unit(seed, 8, knot)+(unit(seed, 8, knot+1)-unit(seed, 8, knot))*e;
    const double gust = noise*std::clamp(.2+s.wind_speed/20, 0.0, 1.0);
    return {{"season",season_names[weather_season(weather_minute())]},{"kind",kind_names[s.kind]},
            {"wind",Json::array({round_to(std::cos(s.wind_angle),1000),round_to(std::sin(s.wind_angle),1000),round_to(s.wind_speed,100)})},
            {"gust",round_to(gust,1000)},{"precipitation",round_to(s.precipitation,1000)},{"cloud",round_to(s.cloud,1000)},{"fog",round_to(s.fog,1000)},
            {"wetness",round_to(s.wetness,1000)},{"snow",round_to(s.snow,1000)},{"temperature",round_to(s.temperature,10)}};
}

// Admin "weather" (arguments validated by admin_world_arguments): kind/season force a weather kind or season ("auto" returns
// to the simulation), minutes bounds the override in real minutes (absent: until "auto"), instant skips the transition ramps.
Json World::weather_admin(const Json& arguments) {
    if (!config_.contains("weather")) throw std::invalid_argument("weather: This gameplay configuration has no weather");
    auto& s = weather_;
    if (arguments.contains("kind")) s.forced_kind = index_of(kind_names, arguments.at("kind").get<std::string>());
    if (arguments.contains("season")) s.forced_season = index_of(season_names, arguments.at("season").get<std::string>());
    s.forced_until = (s.forced_kind >= 0 || s.forced_season >= 0) && arguments.contains("minutes") ? tick_+arguments.at("minutes").get<uint32_t>()*1200u : 0;
    if (arguments.value("instant", false)) {
        const double transition = config_.at("weather").at("transition_minutes").get<double>();
        s.temperature = weather_target(weather_minute()).temperature;
        for (int i = 0; i < 2; ++i) weather_step(weather_minute(), 2*transition);
    }
    return {{"ok",true},{"weather",weather_json()},{"forced",weather_forced()}};
}
Json World::weather_forced() const {
    const auto& s = weather_;
    return {{"kind",s.forced_kind < 0 ? Json(nullptr) : Json(kind_names[s.forced_kind])},{"season",s.forced_season < 0 ? Json(nullptr) : Json(season_names[s.forced_season])},
            {"until_tick",s.forced_until ? Json(s.forced_until) : Json(nullptr)}};
}
}
