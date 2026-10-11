#include "ikv/audio.hpp"
#include "ikv/effects.hpp"
#include "ikv/client_charsel.hpp"
#include "ikv/client_session.hpp"
#include "ikv/client_mutation.hpp"
#include "ikv/client_trade.hpp"
#include "ikv/client_ui.hpp"
#include "ikv/renderer.hpp"
#include "ikv/scene.hpp"
#include <SDL3/SDL.h>
#include <openssl/crypto.h>
#include <turbojpeg.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <iterator>
#include <utility>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
class ProgressConnectionError final:public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};
volatile std::sig_atomic_t interrupted = 0;
void interrupt_handler(int) { interrupted = 1; }

int number(std::string_view text, int lo, int hi) {
    int value = 0;
    const auto [end,error] = std::from_chars(text.data(),text.data()+text.size(),value);
    if (error != std::errc{} || end != text.data()+text.size() || value < lo || value > hi)
        throw std::invalid_argument("invalid numeric option");
    return value;
}
// FSR 3.1.4 presets as saved/CLI names, in ikv::FsrQuality order.
constexpr std::array<std::string_view,5> fsr_modes{"native","quality","balanced","performance","ultra_performance"};
ikv::FsrQuality fsr_quality(std::string_view mode) {
    const auto found=std::find(fsr_modes.begin(),fsr_modes.end(),mode);
    if (found==fsr_modes.end()) throw std::invalid_argument("--fsr-mode requires native|quality|balanced|performance|ultra_performance");
    return static_cast<ikv::FsrQuality>(found-fsr_modes.begin());
}
double decimal_number(std::string_view text,const char* requirement) {
    double value=0;
    const auto [end,error]=std::from_chars(text.data(),text.data()+text.size(),value);
    if (error!=std::errc{} || end!=text.data()+text.size())
        throw std::invalid_argument(requirement);
    return value;
}
std::uint64_t character_number(std::string_view text) {
    std::uint64_t value=0;
    const auto [end,error]=std::from_chars(text.data(),text.data()+text.size(),value);
    if (error!=std::errc{} || end!=text.data()+text.size() || !value || value>INT64_MAX)
        throw std::invalid_argument("invalid character identifier");
    return value;
}
Json read_json(const std::filesystem::path& path, std::uintmax_t cap) {
    if (std::filesystem::file_size(path)>cap) throw std::runtime_error("JSON file exceeds limit");
    std::ifstream file(path);
    if (!file) throw std::runtime_error("cannot open JSON file");
    return Json::parse(file);
}
struct Options {
    std::string host,renderer="opengl",appearance="original",rt="off",rt_style="faithful",auth;
    std::filesystem::path ca,assets,settings,screenshot,commands,audio,model_map,sound_map;
    std::uint16_t port=7000;
    std::uint64_t character=0;
    int width=1280,height=800,dx=0,dy=0,ticks=0;
    bool headless=false,session_stdin=false,fullscreen=false;
    float volume=.5f,music_volume=1; // master device gain; original MusicVolume (music + ambient beds)
    // Ayarlar sliders 3-7 (original globals 0x5eabc0..0x5eabdc, 0..1): Karakter Detay Mes., Görüş Mesafesi (fog distance
    // scale, floor .1), Ortam Efektleri / Karakter Efektleri (particle range² = pref·160000), Ölü Yok Olma Süresi.
    std::array<float,5> prefs{1,1,1,1,1};
    double render_scale=1;
    // Vulkan upscaler: bilinear (render_scale) or fsr3 (AMD FSR 3.1.4, fsr_mode preset + RCAS sharpness 0..1).
    std::string upscaler="bilinear",fsr_mode="quality";
    double fsr_sharpness=.5;
    bool frame_generation=false; // FSR 3.1.4 frame generation (Vulkan + fsr3 only)
    // Original Doku Detay / Gölge Detay prefs (004619d0): 0 = *_4.DAT, 1 = *_2.DAT, 2 = *_1.DAT; launcher >=1700 MB defaults.
    int texture_detail=2,lightmap_detail=1;
    ikv::Camera camera;
};
Options parse_options(int argc,char** argv) {
    Options o;
    for (int i=1;i<argc;++i) if (std::string_view(argv[i])=="--settings") {
        if (++i==argc) throw std::invalid_argument("missing settings path");
        o.settings=argv[i];
    }
    if (!o.settings.empty() && std::filesystem::exists(o.settings)) {
        const auto s=read_json(o.settings,65536);
        if (!s.is_object()) throw std::invalid_argument("settings must be an object");
        o.width=s.value("width",o.width); o.height=s.value("height",o.height);
        o.fullscreen=s.value("fullscreen",false); o.volume=s.value("volume",o.volume); o.music_volume=s.value("music_volume",o.music_volume);
        o.renderer=s.value("renderer",o.renderer); o.appearance=s.value("appearance",o.appearance);
        o.rt=s.value("rt",o.rt); o.rt_style=s.value("rt_style",o.rt_style);
        if (s.contains("render_scale") && !s.at("render_scale").is_number())
            throw std::invalid_argument("saved render_scale requires a finite number in [.25,1]");
        o.render_scale=s.value("render_scale",o.render_scale);
        if ((s.contains("upscaler") && !s.at("upscaler").is_string()) || (s.contains("fsr_mode") && !s.at("fsr_mode").is_string()) ||
            (s.contains("fsr_sharpness") && !s.at("fsr_sharpness").is_number()) ||
            (s.contains("frame_generation") && !s.at("frame_generation").is_boolean()))
            throw std::invalid_argument("saved upscaler/fsr_mode/fsr_sharpness/frame_generation have invalid types");
        o.upscaler=s.value("upscaler",o.upscaler); o.fsr_mode=s.value("fsr_mode",o.fsr_mode);
        o.fsr_sharpness=s.value("fsr_sharpness",o.fsr_sharpness); o.frame_generation=s.value("frame_generation",o.frame_generation);
        if ((s.contains("texture_detail") && !s.at("texture_detail").is_number_integer()) ||
            (s.contains("lightmap_detail") && !s.at("lightmap_detail").is_number_integer()))
            throw std::invalid_argument("saved texture_detail/lightmap_detail require an integer in 0..2");
        o.texture_detail=s.value("texture_detail",o.texture_detail); o.lightmap_detail=s.value("lightmap_detail",o.lightmap_detail);
        if (s.contains("camera")) {
            const auto& c=s.at("camera");
            o.camera.yaw=c.value("yaw",o.camera.yaw); o.camera.pitch=c.value("pitch",o.camera.pitch);
            o.camera.distance=c.value("distance",o.camera.distance);
        }
        if (s.contains("preferences")) o.prefs=s.at("preferences").get<std::array<float,5>>();
    }
    for (int i=1;i<argc;++i) {
        const std::string_view flag=argv[i];
        if (flag=="--help") {
            std::cout << "İKV independent native client (custom authenticated TLSv2; not official protocol)\n"
                "--host HOST --port PORT --ca PEM --session-stdin (--character ID | --auth HTTPS_ORIGIN)\n"
                "  --auth: graphical in-client character selection/creation over the account service's HTTPS API (same CA)\n"
                "[--headless --ticks N --commands JSON --dx -1|0|1 --dy -1|0|1]\n"
                "[--asset-root DIR --renderer opengl|vulkan --appearance original|enhanced]\n"
                "[--model-map JSON --sound-map JSON --audio ORIGINAL_OGG (replaces zone music)]\n"
                "[--settings JSON --width N --height N --fullscreen --screenshot PPM --rt off|auto|required --render-scale .25..1 (Vulkan only)]\n"
                "[--rt-style faithful|physical]  RT lighting: original equation + RT actor shadows (default), or physically based GI\n"
                "[--upscaler bilinear|fsr3 --fsr-mode native|quality|balanced|performance|ultra_performance --fsr-sharpness 0..1]\n"
                "  fsr3: AMD FidelityFX Super Resolution 3.1.4 temporal upscaling (Vulkan only; replaces --render-scale)\n"
                "[--frame-generation on|off]  FSR 3.1.4 frame generation (Vulkan + --upscaler fsr3; default off)\n"
                "[--texture-detail 0|1|2 --lightmap-detail 0|1|2]  original texture/lightmap packs _4/_2/_1 (default 2 and 1)\n"
                "Ground click moves; WASD/arrows control camera; F1 displays recovered Turkish help.\n"
                "Graphical scene, grid and character appearance follow the admitted authoritative source registry.\n";
            std::exit(0);
        }
        if (flag=="--headless") { o.headless=true; continue; }
        if (flag=="--session-stdin") { o.session_stdin=true; continue; }
        if (flag=="--fullscreen") { o.fullscreen=true; continue; }
        if (flag!="--host" && flag!="--port" && flag!="--ca" && flag!="--character" && flag!="--auth" &&
            flag!="--asset-root" && flag!="--renderer" && flag!="--appearance" && flag!="--rt" && flag!="--rt-style" && flag!="--render-scale" &&
            flag!="--upscaler" && flag!="--fsr-mode" && flag!="--fsr-sharpness" && flag!="--frame-generation" &&
            flag!="--texture-detail" && flag!="--lightmap-detail" &&
            flag!="--settings" && flag!="--width" && flag!="--height" && flag!="--ticks" &&
            flag!="--commands" && flag!="--screenshot" && flag!="--audio" && flag!="--dx" && flag!="--dy" &&
            flag!="--model-map" && flag!="--sound-map")
            throw std::invalid_argument("unknown option");
        if (++i==argc) throw std::invalid_argument("missing option value");
        const std::string_view value=argv[i];
        if (flag=="--host") o.host=value;
        else if (flag=="--auth") o.auth=value;
        else if (flag=="--port") o.port=static_cast<std::uint16_t>(number(value,1,65535));
        else if (flag=="--ca") o.ca=value;
        else if (flag=="--character") o.character=character_number(value);
        else if (flag=="--asset-root") o.assets=value;
        else if (flag=="--renderer") o.renderer=value;
        else if (flag=="--appearance") o.appearance=value;
        else if (flag=="--rt") o.rt=value;
        else if (flag=="--rt-style") o.rt_style=value;
        else if (flag=="--render-scale") o.render_scale=decimal_number(value,"--render-scale requires a finite number in [.25,1]");
        else if (flag=="--upscaler") o.upscaler=value;
        else if (flag=="--fsr-mode") o.fsr_mode=value;
        else if (flag=="--fsr-sharpness") o.fsr_sharpness=decimal_number(value,"--fsr-sharpness requires a finite number in [0,1]");
        else if (flag=="--frame-generation") {
            if (value!="on" && value!="off") throw std::invalid_argument("--frame-generation requires on|off");
            o.frame_generation=value=="on";
        }
        else if (flag=="--texture-detail") o.texture_detail=number(value,0,2);
        else if (flag=="--lightmap-detail") o.lightmap_detail=number(value,0,2);
        else if (flag=="--settings") o.settings=value;
        else if (flag=="--commands") o.commands=value;
        else if (flag=="--screenshot") o.screenshot=value;
        else if (flag=="--audio") o.audio=value;
        else if (flag=="--model-map") o.model_map=value;
        else if (flag=="--sound-map") o.sound_map=value;
        else if (flag=="--width") o.width=number(value,640,3840);
        else if (flag=="--height") o.height=number(value,480,2160);
        else if (flag=="--ticks") o.ticks=number(value,1,INT32_MAX);
        else if (flag=="--dx") o.dx=number(value,-1,1);
        else if (flag=="--dy") o.dy=number(value,-1,1);
    }
    if (!std::isfinite(o.render_scale) || o.render_scale<.25 || o.render_scale>1)
        throw std::invalid_argument("render_scale requires a finite number in [.25,1]");
    if (o.renderer=="opengl" && o.render_scale!=1)
        throw std::invalid_argument("OpenGL requires render_scale=1; select Vulkan for render scaling");
    if (o.upscaler!="bilinear" && o.upscaler!="fsr3") throw std::invalid_argument("unknown upscaler");
    (void)fsr_quality(o.fsr_mode);
    if (!std::isfinite(o.fsr_sharpness) || o.fsr_sharpness<0 || o.fsr_sharpness>1)
        throw std::invalid_argument("fsr_sharpness requires a finite number in [0,1]");
    if (o.renderer=="opengl" && (o.upscaler!="bilinear" || o.frame_generation))
        throw std::invalid_argument("OpenGL renders natively; select Vulkan for FSR 3.1.4");
    if (o.texture_detail<0 || o.texture_detail>2 || o.lightmap_detail<0 || o.lightmap_detail>2)
        throw std::invalid_argument("texture_detail/lightmap_detail require an integer in 0..2");
    if (o.frame_generation && o.upscaler!="fsr3") throw std::invalid_argument("--frame-generation on requires --upscaler fsr3");
    if (o.host.empty() || o.ca.empty() || !o.session_stdin || (!o.character && (o.headless || o.auth.empty())))
        throw std::invalid_argument("explicit host, CA, --session-stdin and --character (or graphical --auth) are required");
    if (o.host.size()>253 || o.host.find_first_of("/\\\r\n\t ")!=std::string::npos)
        throw std::invalid_argument("invalid configured host");
    if (o.renderer!="opengl" && o.renderer!="vulkan") throw std::invalid_argument("unknown renderer");
    if (o.appearance!="original" && o.appearance!="enhanced") throw std::invalid_argument("unknown appearance");
    if (o.rt!="off" && o.rt!="auto" && o.rt!="required") throw std::invalid_argument("unknown RT policy");
    if (o.rt_style!="faithful" && o.rt_style!="physical") throw std::invalid_argument("unknown RT style");
    if (o.width<640 || o.width>3840 || o.height<480 || o.height>2160 || !std::isfinite(o.volume) || o.volume<0 || o.volume>1 ||
        !std::isfinite(o.music_volume) || o.music_volume<0 || o.music_volume>1 ||
        std::any_of(o.prefs.begin(),o.prefs.end(),[](float p){return !std::isfinite(p) || p<0 || p>1;}))
        throw std::invalid_argument("invalid saved graphics/audio settings");
    ikv::validate_camera(o.camera);
    if (o.dx && o.dy) throw std::invalid_argument("headless intent must be cardinal");
    if (!o.headless && (o.dx || o.dy)) throw std::invalid_argument("native movement uses ground clicks, not keyboard intent");
    if (!o.headless && o.assets.empty()) throw std::invalid_argument("graphical client requires original --asset-root");
    if (o.headless && !o.screenshot.empty()) throw std::invalid_argument("screenshot requires a native framebuffer");
    return o;
}
struct Secret {
    std::array<char,64> bytes{};
    Secret() {
        try {
        for (auto& c:bytes) {
            const int value=std::cin.get();
            if (value==EOF || !((value>='0' && value<='9') || (value>='a' && value<='f')))
                throw std::invalid_argument("stdin session must be exactly 64 lowercase hexadecimal characters");
            c=static_cast<char>(value);
        }
        const int end=std::cin.get();
        if (end!='\n' && end!=EOF) throw std::invalid_argument("stdin session must be one bounded line");
        } catch (...) { OPENSSL_cleanse(bytes.data(),bytes.size()); throw; }
    }
    ~Secret() { OPENSSL_cleanse(bytes.data(),bytes.size()); }
    std::string_view view() const { return {bytes.data(),bytes.size()}; }
};
struct Script {
    Json entries=Json::array();
    std::size_t next=0;
    explicit Script(const std::filesystem::path& path) {
        if (path.empty()) return;
        entries=read_json(path,1024*1024);
        if (!entries.is_array() || entries.size()>4096) throw std::invalid_argument("commands must be a bounded array");
        int previous=0;
        for (const auto& e:entries) {
            if (!e.is_object() || !e.contains("tick") || !e.at("tick").is_number_integer())
                throw std::invalid_argument("command requires integer relative snapshot tick");
            const int tick=e.at("tick").get<int>();
            if (tick<1 || tick<previous || e.contains("action")+e.contains("destination")+e.contains("effect")+e.contains("click")!=1)
                throw std::invalid_argument("commands must be sorted and contain one action, destination, effect or click");
            if (e.contains("click") && (!e.at("click").is_array() || e.at("click").size()!=2 || !e.at("click")[0].is_number() || !e.at("click")[1].is_number()))
                throw std::invalid_argument("click command requires [x, y] drawable pixels");
            if (e.contains("effect") && (!e.at("effect").is_string() || e.at("effect").get<std::string>().size()>64))
                throw std::invalid_argument("effect command requires a bounded effects.xml name");
            if (e.contains("action") && (!e.at("action").is_object() || !e.at("action").contains("action")))
                throw std::invalid_argument("typed action object required");
            if (e.contains("destination")) {
                const auto& d=e.at("destination");
                if (!d.is_object() || !d.contains("x") || !d.contains("y") || !d.at("x").is_number_integer() || !d.at("y").is_number_integer() ||
                    (d.at("x").is_number_unsigned() && d.at("x").get<std::uint64_t>()>static_cast<std::uint64_t>(ikv::coordinate_limit)) ||
                    (d.at("y").is_number_unsigned() && d.at("y").get<std::uint64_t>()>static_cast<std::uint64_t>(ikv::coordinate_limit)) ||
                    d.at("x").get<std::int64_t>() < -ikv::coordinate_limit || d.at("x").get<std::int64_t>() > ikv::coordinate_limit ||
                    d.at("y").get<std::int64_t>() < -ikv::coordinate_limit || d.at("y").get<std::int64_t>() > ikv::coordinate_limit)
                    throw std::invalid_argument("destination exceeds custom protocol coordinate limit");
            }
            previous=tick;
        }
    }
    // "effect": local visual test trigger on the player (original effects.xml/particle_systems.xml name);
    // "click": a left click at drawable pixel [x, y] through the normal picking path.
    template<class Effect,class Click> void run(int count,ikv::ClientSession& session,Effect&& effect,Click&& click) {
        while (next<entries.size() && entries[next].at("tick").get<int>()<=count) {
            const auto& e=entries[next++];
            if (e.contains("action")) session.action(e.at("action"));
            else if (e.contains("effect")) effect(e.at("effect").get<std::string>());
            else if (e.contains("click")) click(e.at("click")[0].get<float>(),e.at("click")[1].get<float>());
            else session.destination(e.at("destination").at("x").get<std::int32_t>(),e.at("destination").at("y").get<std::int32_t>());
        }
    }
};
void print_snapshot(const ikv::ClientSession& s) {
    Json players=Json::array();
    for (const auto& p:s.players) players.push_back({{"id",p.id},{"x",p.x},{"y",p.y}});
    std::cout << Json{{"self",s.self},{"tick",s.tick},{"players",std::move(players)}}.dump() << '\n' << std::flush;
    if (!std::cout) throw std::runtime_error("snapshot output failed");
}
int headless(const Options& o,const Secret& secret) {
    ikv::ClientSession session(o.host,o.port,o.ca,secret.view(),o.character);
    Script script(o.commands);
    session.intent(o.dx,o.dy);
    int count=0; bool finishing=false;
    while (!finishing || !session.drained()) {
        if (interrupted && !finishing) { finishing=true; session.stop(); }
        session.pump(10,[&](ikv::Type type) {
            if (type==ikv::Type::snapshot) {
                ++count; print_snapshot(session);
                if (!finishing) script.run(count,session,[](const std::string&){},[](float,float){}); // headless: no visuals
                if (o.ticks && count>=o.ticks && !finishing) { finishing=true; session.stop(); }
            } else if (type==ikv::Type::state) {
                std::cout << Json{{"type","state"},{"state",session.state}}.dump() << '\n' << std::flush;
                if (!std::cout) throw std::runtime_error("state output failed");
            }
        });
        if (session.eof && !finishing) {
            if (!session.self || (o.ticks && count<o.ticks)) throw std::runtime_error("server closed before requested world state");
            finishing=true; session.stop();
        }
    }
    return 0;
}

struct Hit { int x,y,w,h; Json action; int mode=-1; std::string value; };
const Json& empty_object() { static const Json object=Json::object(); return object; }
const Json& character_state(const Json& state) {
    if (!state.is_object() || !state.contains("character")) return empty_object();
    const auto& c=state.at("character");
    if (!c.is_object() || !c.contains("state") || !c.at("state").is_object()) return empty_object();
    return c.at("state");
}
std::string item_name(const Json& state,const std::string& id) {
    if (state.contains("catalogs")) {
        const auto& catalogs=state.at("catalogs");
        if (catalogs.contains("items") && catalogs.at("items").contains(id))
            return catalogs.at("items").at(id).value("name",id);
    }
    return id;
}
// Original data\prefixes.dat as the client loader 0043aac0 keeps it: name, effect_name and attreffects (attribute id, value).
struct SourcePrefix { std::string name,effect; std::vector<std::pair<int,double>> attributes; };
std::map<int,SourcePrefix> source_prefixes;
// Efsun prefixes of an inventory entry: its own instance prefixes, else the item's fixed classes.dat prefix1/prefix2.
Json entry_prefixes(const Json& state,const Json& entry) {
    if (entry.contains("prefixes")) return entry.at("prefixes");
    const auto& catalogs=state.value("catalogs",Json::object());
    const auto id=entry.value("item",std::string{});
    if (catalogs.contains("items") && catalogs.at("items").contains(id)) return catalogs.at("items").at(id).value("prefixes",Json::array());
    return Json::array();
}
// Tooltip title 0x437590: prefix1 + " " + prefix2 + " " + item name (VOD 34:00 "Savunma Artırıcı Kanvaslı Deri Eldiven").
std::string entry_name(const Json& state,const Json& entry) {
    std::string name;
    for (const auto& p:entry_prefixes(state,entry)) if (const auto f=source_prefixes.find(p.get<int>()); f!=source_prefixes.end()) name+=f->second.name+" ";
    return name+item_name(state,entry.value("item",std::string{}));
}
// Conservative allocated-payload accounting; never serialize a state merely to measure it.
std::size_t json_bound(const Json& value) {
    std::size_t bytes=sizeof(Json);
    if (value.is_string()) {
        const auto& text=value.get_ref<const std::string&>();
        return bytes+sizeof(std::string)+text.capacity()+33;
    }
    if (value.is_object()) {
        bytes+=sizeof(Json::object_t)+32;
        for (const auto& [key,item]:value.items())
            bytes+=sizeof(Json::object_t::value_type)+4*sizeof(void*)+32+key.capacity()+1+json_bound(item);
    } else if (value.is_array()) {
        const auto& array=value.get_ref<const Json::array_t&>();
        bytes+=sizeof(Json::array_t)+32+array.capacity()*sizeof(Json);
        for (const auto& item:array) bytes+=json_bound(item);
    }
    return bytes;
}
std::array<float,3> transform_point(const ikv::Transform& t,const ikv::Vertex& v) {
    const auto& m=t.matrix;
    return {m[0]*v.x+m[4]*v.y+m[8]*v.z+m[12],m[1]*v.x+m[5]*v.y+m[9]*v.z+m[13],m[2]*v.x+m[6]*v.y+m[10]*v.z+m[14]};
}
std::array<float,3> subtract(const std::array<float,3>& a,const std::array<float,3>& b) { return {a[0]-b[0],a[1]-b[1],a[2]-b[2]}; }
std::array<float,3> cross(const std::array<float,3>& a,const std::array<float,3>& b) { return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]}; }
float dot(const std::array<float,3>& a,const std::array<float,3>& b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
// Nearest ray parameter against one recovered source collider class (walk set or 'nocamera' blockers).
template<class Accept>
std::optional<float> source_ray(const ikv::Scene& scene,bool support,const std::array<float,3>& origin,const std::array<float,3>& ray,Accept accept) {
    float nearest=std::numeric_limits<float>::max();
    std::optional<float> result;
    // ponytail: O(triangles) per query (clicks, one ~7k-triangle camera ray per frame); spatial ray index if profiling demands it.
    for (const auto& instance:scene.instances) {
        if (instance.entity_id || !(support?instance.source_support:instance.source_nocamera) || instance.mesh>=scene.meshes.size()) continue;
        const auto& mesh=scene.meshes[instance.mesh];
        for (std::size_t i=0;i+2<mesh.indices.size();i+=3) {
            if (mesh.indices[i]>=mesh.vertices.size() || mesh.indices[i+1]>=mesh.vertices.size() || mesh.indices[i+2]>=mesh.vertices.size()) continue;
            const auto a=transform_point(instance.transform,mesh.vertices[mesh.indices[i]]);
            const auto b=transform_point(instance.transform,mesh.vertices[mesh.indices[i+1]]);
            const auto c=transform_point(instance.transform,mesh.vertices[mesh.indices[i+2]]);
            const auto ab=subtract(b,a),ac=subtract(c,a);
            const auto p=cross(ray,ac); const float determinant=dot(ab,p);
            if (std::abs(determinant)<1e-8f) continue;
            const float inverse=1/determinant; const auto t=subtract(origin,a);
            const float u=dot(t,p)*inverse; if (u<0 || u>1) continue;
            const auto q=cross(t,ab); const float v=dot(ray,q)*inverse; if (v<0 || u+v>1) continue;
            const float distance=dot(ac,q)*inverse;
            if (distance>=0 && distance<nearest && accept(distance)) { nearest=distance; result=distance; }
        }
    }
    return result;
}
// Eye and (unnormalised) direction of the camera ray through pixel (px,py).
std::pair<std::array<float,3>,std::array<float,3>> camera_ray(const ikv::Camera& camera,float px,float py,int w,int h) {
    const auto basis=ikv::camera_basis(camera);
    const float scale=std::tan(camera.fov*.008726646259971648f);
    const float rx=(2*px/static_cast<float>(w)-1)*scale*static_cast<float>(w)/static_cast<float>(h);
    const float ry=(1-2*py/static_cast<float>(h))*scale;
    std::array<float,3> ray{};
    for (unsigned i=0;i<3;++i) ray[i]=basis.forward[i]+rx*basis.right[i]+ry*basis.up[i];
    return {basis.eye,ray};
}
// Ray intersections use only the recovered SourceSupport walk set and the same height grid as authority.
std::optional<std::array<float,3>> pick_scene(const ikv::Scene& scene,const ikv::Camera& camera,float px,float py,int w,int h) {
    if (w<=0 || h<=0 || !scene.source_registry_id) return {};
    const auto [eye,ray]=camera_ray(camera,px,py,w,h);
    const auto hit=source_ray(scene,true,eye,ray,[&](float d){return ikv::zone_height(scene.zone,eye[0]+ray[0]*d,eye[2]+ray[2]*d).has_value();});
    if (!hit) return {};
    return std::array<float,3>{eye[0]+ray[0]**hit,eye[1]+ray[1]**hit,eye[2]+ray[2]**hit};
}
// Slab test of a ray against an axis-aligned box: entry distance along ray, or nothing (D3DXBoxBoundProbe, 0047cb80).
std::optional<float> ray_box(const std::array<float,3>& eye,const std::array<float,3>& ray,const std::array<float,3>& lo,const std::array<float,3>& hi) {
    float enter=0,leave=INFINITY;
    for (unsigned i=0;i<3;++i) {
        if (ray[i]==0) { if (eye[i]<lo[i] || eye[i]>hi[i]) return {}; continue; }
        float a=(lo[i]-eye[i])/ray[i],b=(hi[i]-eye[i])/ray[i];
        if (a>b) std::swap(a,b);
        enter=std::max(enter,a); leave=std::min(leave,b);
        if (enter>leave) return {};
    }
    return enter;
}
// Source 'nocamera' colliders keep the third-person eye outside buildings: pull it in front of the first blocker.
float unoccluded_distance(const ikv::Scene& scene,const ikv::Camera& camera) {
    const auto forward=ikv::camera_basis(camera).forward;
    const auto hit=source_ray(scene,false,camera.target,{-forward[0],-forward[1],-forward[2]},[&](float d){return d<camera.distance;});
    return hit?std::max(1.f,*hit-1):camera.distance;
}

class Application {
    Options options_;
    const Secret& secret_;
    ikv::Scene scene_;
    SDL_Window* window_=nullptr;
    std::unique_ptr<ikv::Renderer> renderer_;
    std::unique_ptr<ikv::ClientUI> ui_;
    std::unique_ptr<ikv::GameAudio> audio_;
    std::unique_ptr<ikv::EffectSystem> effects_;
    std::unordered_map<std::uint32_t,std::uint32_t> effect_cues_; // entity -> last seen ActorAnimation::audio_cue
    std::vector<std::string> scripted_effects_; // --commands "effect" entries, fired once the player is drawn
    std::unique_ptr<ikv::ClientSession> session_;
    // Transport owns its live state; presentation is immutable while a cold CPU/GPU load runs.
    Json state_=Json::object();
    std::vector<ikv::Player> players_;
    bool has_snapshot_=false,state_pending_=false;
    std::optional<unsigned> accepted_source_,snapshot_source_;
    std::uint64_t snapshot_revision_=0,painted_snapshot_revision_=0;
    using PendingMutation=ikv::PendingMutation;
    std::vector<std::shared_ptr<const Json>> accepted_actors_;
    std::deque<PendingMutation> pending_mutations_;
    std::size_t pending_bytes_=0;
    Script script_;
    std::vector<Hit> hits_;
    int hover_hit_=-1; // topmost hit under the pointer at the last motion; a change (or a shown tip) repaints the HUD
    std::vector<std::pair<std::string,std::array<std::uint8_t,4>>> tip_; // hovered window's tooltip lines, drawn last
    // Drag-and-drop sources painted this frame (skill icons, bag items, quickbar slots) and the pressed/dragged payload.
    std::vector<std::pair<std::array<int,4>,Json>> drags_;
    Json drag_; bool dragging_=false; float press_x_=0,press_y_=0,drag_x_=0,drag_y_=0; std::string drag_trace_;
    Clock::time_point slot_ready_{};
    std::vector<ikv::Player> rendered_,entities_;
    std::vector<ikv::RenderEntity> poses_;
    // Local time of server tick 0, from the least-delayed arrival; actors render a fixed delay behind it.
    std::optional<Clock::time_point> tick_epoch_;
    std::uint32_t epoch_tick_=0;
    Clock::time_point last_frame_=Clock::now();
    std::uint32_t target_=0;
    int width_=0,height_=0,count_=0,channel_=3,help_page_=0; // channel_: chat_channels index, Söyle default
    std::string help_book_; // helps.xml book opened by a '?' button or tutorial trigger (paging stays inside it); empty = F1, all pages
    std::uint32_t tutorial_target_=0; bool tutorial_entered_=false; // Seviye1a/1b triggers (004b63b4, 0041b24f)
    // HUD: original 1080p atlas pixels scaled by s_; right-side windows are toggled, oldest first.
    enum Window { win_bag=1,win_quests,win_vendor,win_trade,win_group,win_settings,win_guild,win_help,win_character,win_skills,win_craft,win_bank,win_contacts };
    float s_=1;
    int ui_px_=0,context_=0,chat_scroll_=0,chat_extra_=0;
    int contacts_tab_=0,system_menu_=0; // Bağlantılar list (0 Arkadaşlar, 1 Yasaklılar); Esc menu (1 open)
    Json confirm_; std::string confirm_text_; // original "Dikkat" yes/no box: action (or "quit") sent on ✓
    std::string split_amount_; // digits typed into the 0042f690 split box while confirm_ is a split (sent by 004336c0 on Enter)
    std::array<float,5> prefs_saved_{}; // Ayarlar ✗ restores prefs_saved_
    std::vector<int> windows_;
    std::array<int,win_contacts+1> scroll_{};
    std::vector<std::pair<int,std::array<int,4>>> window_rects_;
    std::array<int,4> chat_rect_{};
    struct LogLine { std::string text; std::array<std::uint8_t,4> color; int channel=chat_system; std::uint32_t from=0; std::string name; Json link; }; // channel: chat_channels index; from/name: chat sender (0040aad0 name menu); link: {state,entry} of a Ctrl-click item link (0040aa50)
    std::deque<LogLine> log_;
    Clock::time_point link_ready_{}; // 0042e8c0 2000 ms item-link guard (0x611440)
    // Guide "Eşya Yansıtma": right-clicking a linked item name in chat shows all its properties (0040aa50 link on the line).
    std::vector<std::pair<std::array<int,4>,std::size_t>> chat_link_hits_; Json chat_link_open_; int chat_link_x_=0,chat_link_y_=0;
    std::string flash_;
    std::array<std::uint8_t,4> flash_color_{238,205,129,255}; // gold notices; 0xffff7777 for the original red refusals (0048b660)
    Clock::time_point flash_until_{};
    struct Label {
        std::uint32_t id=0;
        int x=0,head=0,feet=0; // screen x, head y, feet y (body pick)
        int tx=0,ty=0; // 0047c530 tag anchor (feet + 7.0); the name sits 14 px + boundy·0.3·depth scale above it
        bool name=false; // draw the name: only other PCs within 170 units (00471a00..00471c9b); every entry stays a body pick
        bool operator==(const Label&) const=default;
    };
    std::vector<Label> labels_;
    Json group_members_=Json::array(); std::uint32_t group_leader_=0;
    bool quit_=false,disconnected_=false,editing_=false,reconnect_=false,captured_=false;
    std::string screenshot_request_; // PrintScreen file name, written after the next rendered frame (00468010)
    bool hud_dirty_=true;
    bool world_dirty_=false; // only name labels/floaters changed: paint_world alone (hud_dirty_ repaints both)
    std::size_t label_hits_=0; // hits_ prefix painted by paint_labels
    int clock_minute_=-1; // last painted server clock minute
    Json hud_values_=Json::object(),hud_entities_=Json::array();
    std::string input_,selected_uid_;
    std::uint64_t trade_currency_=0;
    float elapsed_=0;
    Json trade_items_=Json::array();
    ikv::ClientTrade trade_;
    std::uint64_t trade_seen_revision_=0,trade_seen_rows_=0,trade_pending_rows_=0,trade_painted_revision_=0;
    std::string trade_seen_phase_;
    bool trade_pending_present_=false;
    Json model_map_=Json::object(),sound_map_=Json::object();
    struct ActorState {
        struct Sample { std::uint32_t tick=0; std::int32_t x=0,z=0; };
        std::uint32_t id=0,source_class=0;
        Json binding;
        std::array<Sample,4> history{}; // Authoritative positions, oldest first.
        std::size_t samples=0;
        int hp=-1;
        float yaw=0,shown_yaw=0;
        bool moving=false,alive=true,locomotion_dirty=false,node=false; // node: gathering node (loc cliplist maden_Wait 1 / picked 3)
        std::string shadow; // classes.dat shadowtype particle system (0047aa00); empty/"none" = no shadow
        std::string weapon; // equipped right-hand classes.dat objecttype (weaponlight/heavy/staff/ranged); empty = bare hands
        unsigned pose=2; // that object's `pose` wait clip (00439cf3 parses it; 2 Wait_BareHand, 6/9/10 Wait_*RightHandedWeapons)
        unsigned swing=0; // next variant within this actor's attack/damage clip family
        Clock::time_point fidget_at{}; // next idle variant while resting on the wait clip
        Clock::time_point died_at{}; // death seen (entity +0x1a8 stamp): Ölü Yok Olma Süresi hides the body later
        float height=head_height; // label/floater anchor above the feet: classes.dat bounds box top (see class_bounds)
        std::array<float,3> bounds{}; // classes.dat half extents; the hover/click ray box (entity_at)
        bool skin_offscreen=false; // hold_offscreen_skins: last frame's skin was held because the actor was out of view
        std::uint64_t assembly=0; // serial of the latest model request (attach_models); older worker results are dropped
    };
    std::vector<ActorState> actors_;
    struct AssemblyRequest { std::uint64_t serial=0; std::shared_ptr<const Json> data; std::string member; ikv::SourceAppearance source; };
    struct AssemblyResult { std::uint64_t serial=0; std::shared_ptr<const Json> data; std::optional<ikv::PreparedActor> actor; std::string error; std::exception_ptr fatal; };
    struct AssemblyBatch { std::vector<AssemblyResult> results; std::vector<ikv::Texture> textures; }; // textures: decoded once per batch
    std::vector<AssemblyRequest> assembly_queue_;
    std::uint64_t assembly_serial_=0;
    ikv::Scene actor_context_; // actor tables and decoded models, kept across zones; the worker's alone while assembly_ runs
    std::future<AssemblyBatch> assembly_; // declared after actor_context_, so destruction joins the worker first
    ikv::assets::DdsImage original_ui1_{},original_ui2_{};
    std::vector<std::pair<std::string,ikv::assets::DdsImage>> help_images_;
    std::vector<ikv::SourceSkill> source_skills_;
    std::map<std::string,Clock::time_point> skill_ready_; // own skill cooldowns from accepted skill events
    Clock::time_point skill_global_ready_{},cooldown_until_{},cooldown_repaint_{};
    Clock::time_point resurrect_at_{}; // original 00410b00: death sets a 15000 ms resurrection deadline
    long long resurrect_shown_=-2;
    std::vector<std::uint32_t> sacks_; // attached Ganimet sack models (corpse id - sack_offset)
    std::uint32_t hilite_=0,hover_=0; // entity carrying the GroundHilite selection ring; entity under the cursor (contour)
    int skill_slot_=-1; std::array<int,15> skill_pending_{}; // Yetenekler selection (+0x1d90) and per-slot pending points (+0x19b4)
    std::map<std::string,int> stat_pending_; // Karakter Bilgisi pending Dirençler/Üretim/Toplama points (+0x3b4..+0x4cc), sent by ✓ 00402a80
    int craft_page_=0; std::string craft_selected_,bank_selected_; bool bank_session_=false; // Üretim page/recipe; Banka selection and open session
    std::vector<std::pair<unsigned,int>> source_item_icons_;
    std::map<std::uint32_t,std::vector<std::string>> glows_; // attached prefix effect_name glows per actor
    int help_image_page_=0;
    bool original_ui_cached_=false;
    // Floating stat text, Harita minimap, cursors, loading screen and chat channels (docs/PORTING_GAPS.md #7 #10 #11 #14 #15).
    struct Floater { std::uint32_t id=0; std::string text; std::array<std::uint8_t,4> color{}; Clock::time_point born; float start=0; };
    std::deque<Floater> floaters_;
    std::vector<std::pair<std::uint32_t,int>> floater_hp_; // last presented entity hp
    ikv::assets::DdsImage zone_map_{},eye_{};
    unsigned zone_map_zone_=UINT32_MAX;
    bool map_open_=true,camera_locked_=false,cursors_loaded_=false,loading_=false,chat_filter_open_=false;
    int map_zoom_=1,cursor_=-1,chat_previous_=3;
    std::array<int,2> map_center_{};
    std::array<int,4> map_rect_{};
    Clock::time_point map_painted_{},loading_painted_{};
    std::array<SDL_Cursor*,5> cursors_{};
    struct LoadingArt { ikv::assets::DdsImage art{},logo{}; std::vector<ikv::assets::DdsImage> frames; } loading_art_;
    ikv::Scene loading_scene_; // empty scene: the renderer presents only the loading overlay
    float loading_progress_=0;
    std::array<bool,7> chat_hidden_{}; // 'F' filter; Sistem cannot be filtered
    std::uint32_t whisper_to_=0,whisper_from_=0;
    std::string whisper_name_,whisper_from_name_;
    // Chat A/G toggles (0040d330/0040d220, flags 0x5e98a1/0x5e98a0, both allowed at start): blocked trade requests are
    // declined ('Fqrt', 004a8afc) and blocked group/guild invites are dropped (004a7f40/004a8010).
    bool trade_allowed_=true,invites_allowed_=true;
    std::uint32_t chat_menu_id_=0; std::string chat_menu_name_; // chat-line name menu (0040aad0)
    Json loot_; // open Ganimet window: last 'loot_open' event, taken slots nulled (null = closed)
    Json dialogue_; // last 'dialogue' event of the open NPC Konuş window (null = closed)
    int dialogue_page_=0;
    std::string selected_quest_;
    int quest_text_scroll_=0; // Görevler lower panel (description) scroll
public:
    Application(Options options,const Secret& secret):options_(std::move(options)),secret_(secret),script_(options_.commands) {
        if (!SDL_Init(SDL_INIT_VIDEO|SDL_INIT_AUDIO)) throw std::runtime_error(std::string("SDL initialization failed: ")+SDL_GetError());
        try {
            if (!options_.model_map.empty()) model_map_=read_json(options_.model_map,65536);
            if (!options_.sound_map.empty()) sound_map_=read_json(options_.sound_map,65536);
            if (!model_map_.is_object() || !sound_map_.is_object()) throw std::invalid_argument("model/sound map must be JSON objects");
            for (const auto& [key,member]:model_map_.items()) {
                if (!key.starts_with("id:") || character_number(std::string_view(key).substr(3))>UINT32_MAX || !member.is_string())
                    throw std::invalid_argument("operator model mapping requires explicit id:N member strings");
            }
            if (options_.renderer=="opengl" &&
                (!SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3) || !SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,3) ||
                 !SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_CORE) ||
                 !SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER,1) || !SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE,24)))
                throw std::runtime_error("OpenGL window attributes unavailable");
#if IKV_HAS_VULKAN
            const SDL_WindowFlags provider=options_.renderer=="opengl" ? SDL_WINDOW_OPENGL : SDL_WINDOW_VULKAN;
#else
            const SDL_WindowFlags provider=SDL_WINDOW_OPENGL;
#endif
            window_=SDL_CreateWindow("İKV — Bağımsız Topluluk Dünyası",options_.width,options_.height,
                provider|SDL_WINDOW_RESIZABLE|SDL_WINDOW_HIGH_PIXEL_DENSITY|(options_.fullscreen?SDL_WINDOW_FULLSCREEN:0));
            if (!window_) throw std::runtime_error(std::string("native window creation failed: ")+SDL_GetError());
            // Text input stays on for the window's lifetime and editing_ gates it, like the original's always-translated WM_CHAR:
            // starting it on the Enter key drops keys already queued behind that Enter (SDL only makes text while active).
            SDL_StartTextInput(window_);
            reset_renderer();
            hits_.reserve(160); rendered_.reserve(160); poses_.reserve(160); entities_.reserve(128); actors_.reserve(160);
            // Zone music/ambient follow the original grids once the player is placed (GameAudio::frame).
            audio_=std::make_unique<ikv::GameAudio>(asset_base(),options_.volume,options_.music_volume);
            if (!options_.audio.empty()) audio_->music_override(options_.audio);
            try { effects_=std::make_unique<ikv::EffectSystem>(asset_base()); }
            catch (const std::exception& e) { std::cerr << "Özgün parçacık efektleri yüklenemedi: " << e.what() << '\n'; }
            if (!model_map_.empty()) notice("Açık operatör model eşlemesi bağımsızdır; gerçek kaynak PC görünümünün yerine geçer.");
            if (!quit_ && (options_.character || choose_character())) connect();
            last_frame_=Clock::now();
        } catch (...) { shutdown(); throw; }
    }
    ~Application() { shutdown(); }
    int run() {
        while (!quit_ || (session_ && (!session_->drained() || pending_presentation()))) {
            const auto now=Clock::now();
            const float real=std::max(std::chrono::duration<float>(now-last_frame_).count(),0.f); last_frame_=now;
            // Original 0047aa80 advances clips by real milliseconds; only input/camera steps are clamped. A slow frame
            // rate must not stretch Death (4.8 s) across the whole Canlanma Süresi countdown.
            const float dt=std::min(real,.1f); elapsed_+=real;
            input(dt);
            if (interrupted && !quit_) leave();
            if (reconnect_ && !quit_) { reconnect_=false; connect(); }
            if (session_ && !disconnected_) {
                try {
                    pump_network();
                    apply_pending();
                    if (session_->eof && !quit_) disconnect("Sunucu bağlantıyı kapattı. Yeniden bağlanabilir veya başlatıcıda giriş yapabilirsiniz.");
                } catch (const std::exception& e) {
                    if (options_.ticks || !renderer_) throw;
                    disconnect(std::string("Bağlantı hatası: ")+e.what());
                }
            }
            ikv::update_scene(scene_,elapsed_);
            audio_->update();
            try { draw(dt); }
            catch (const ProgressConnectionError& e) {
                if (options_.ticks || !renderer_) throw;
                disconnect(std::string("Bağlantı hatası: ")+e.what());
                // A transport exception can interrupt an overlay/resource replacement.
                // Recreate the same configured provider; never reuse partially committed GPU state.
                reset_renderer();
            }
            if (quit_ && session_ && session_->drained() && !pending_presentation()) break;
            // Presentation (vsync/FIFO) paces frames; only cap a non-blocking present at 250 fps. A fixed sleep here added
            // 8 ms to every frame and pushed 60 Hz frames past the vblank.
            if (const auto spent=Clock::now()-now; spent<std::chrono::milliseconds(4))
                SDL_DelayPrecise(static_cast<Uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::milliseconds(4)-spent).count()));
        }
        if (!options_.screenshot.empty() && !captured_) throw std::runtime_error("no authoritative native frame available for capture");
        save_settings(); return 0;
    }
private:
    void queue_state() {
        const auto& state=session_->state;
        if (state.at("source_registry_id").is_null())
            throw std::runtime_error("graphical play requires an authoritative source scene");
        const auto source=state.at("source_registry_id").get<unsigned>();
        const bool changed_source=!accepted_source_ || *accepted_source_!=source;
        const auto tick=state.at("tick").get<std::uint32_t>();
        PendingMutation mutation; mutation.source=source; mutation.reset=changed_source;
        if (changed_source) accepted_actors_.clear();
        auto visit=[&](std::uint32_t id,const Json& given,const Json& status,bool pc) {
            // Karakter Detay Mes. (0x5eabc0; 004a9614 -> 0043fd40, 0043fe40 in range): another PC farther than pref*170 scene
            // units wears the sex's default set (face/hair 0: K_BST_NIC/E_BST_MEL, SAC_001_SIY; Mavi Kot Pantolon 5211/5219,
            // İnce Deri Ayakkabı 5103/4100, generic body/hands) instead of its equipment. Single threshold: the in-range
            // restore factor [0x77c31c] has no static writer.
            Json simplified;
            if (pc && id!=session_->self) {
                const auto& own=character_state(state);
                if (std::hypot(status.at("x").get<double>()-own.value("x",0.0),status.at("y").get<double>()-own.value("y",0.0))>options_.prefs[0]*170000.0) {
                    const bool male=given.at("appearance").value("sex",1u)==1;
                    simplified=given;
                    for (const char* key:{"face","hair","hair_color","beard","beard_color"}) simplified["appearance"][key]=0;
                    simplified["equipment"]=Json::array({{{"slot",3},{"source_uid",male?5219:5211}},{{"slot",5},{"source_uid",male?4100:5103}}});
                }
            }
            const Json& data=simplified.is_null()?given:simplified;
            const auto old=std::find_if(accepted_actors_.begin(),accepted_actors_.end(),[&](const auto& row){return row->at("id")==id;});
            const int hp=status.at("hp").get<int>();
            const bool alive=pc?hp>0:data.at("alive").get<bool>();
            bool changed=old==accepted_actors_.end();
            if (!changed) {
                const auto& row=**old;
                changed=row.at("hp")!=hp || row.at("alive")!=alive || row.at("pc")!=pc;
                for (const auto* key:{"appearance","equipment","class_id","source_class","source_uid","kind","posture"})
                    if (row.contains(key)!=data.contains(key) ||
                        (data.contains(key) && row.at(key)!=data.at(key))) changed=true;
            }
            if (!changed) return;
            auto row=std::make_shared<Json>(Json{{"id",id},{"pc",pc},{"hp",hp},{"alive",alive},
                {"x",status.at("x")},{"y",status.at("y")}});
            for (const auto* key:{"appearance","equipment","class_id","source_class","source_uid","kind","posture"})
                if (data.contains(key)) (*row)[key]=data.at(key);
            mutation.actors.push_back(row); mutation.ticks.push_back(tick);
            if (old==accepted_actors_.end()) accepted_actors_.push_back(std::move(row));
            else *old=std::move(row);
        };
        visit(session_->self,state.at("character"),character_state(state),true);
        const auto& visible=state.at("entities");
        for (const auto& entity:visible)
            if (entity.at("id")!=session_->self) visit(entity.at("id").get<std::uint32_t>(),entity,entity,entity.at("kind")=="player");
        std::erase_if(accepted_actors_,[&](const auto& row) {
            const auto id=row->at("id").template get<std::uint32_t>();
            const bool removed=id!=session_->self && std::none_of(visible.begin(),visible.end(),[&](const auto& entity){return entity.at("id")==id;});
            if (removed) mutation.removed.push_back(id);
            return removed;
        });
        if (state.contains("events") && !state.at("events").empty()) mutation.events=state.at("events");
        accepted_source_=source; state_pending_=true;
        if (!changed_source && mutation.actors.empty() && mutation.removed.empty() && mutation.events.empty()) return;
        size_mutation(mutation);
        // A GPU zone load (~2 s raster, longer with RT) pumps the network while presentation is frozen and moving actors
        // keep crossing the AOI. Under queue pressure the newest same-zone mutations fold into their predecessor
        // instead of ending the session: the merged one shows the same final rows and plays every event in order. A zone
        // change never folds, so a scene still loads before its actors attach.
        while (pending_mutations_.size()>=128 || mutation.bytes>262144-pending_bytes_) {
            if (pending_mutations_.empty() || mutation.reset || pending_mutations_.back().source!=mutation.source)
                throw std::runtime_error("deferred presentation queue limit exceeded");
            auto older=std::move(pending_mutations_.back());
            pending_mutations_.pop_back(); pending_bytes_-=older.bytes;
            ikv::fold_mutation(older,std::move(mutation)); mutation=std::move(older); size_mutation(mutation);
        }
        pending_bytes_+=mutation.bytes; pending_mutations_.push_back(std::move(mutation));
    }
    static void size_mutation(PendingMutation& mutation) {
        mutation.bytes=sizeof(PendingMutation)+512+mutation.actors.capacity()*sizeof(std::shared_ptr<const Json>)+
            (mutation.ticks.capacity()+mutation.removed.capacity())*sizeof(std::uint32_t)+json_bound(mutation.events);
        for (const auto& row:mutation.actors) mutation.bytes+=json_bound(*row);
    }
    void pump_network() {
        if (!session_ || disconnected_) return;
        session_->pump(0,[&](ikv::Type type) {
            if (type==ikv::Type::welcome) std::cout<<"READY client_self="<<session_->self<<'\n'<<std::flush;
            else if (type==ikv::Type::state) queue_state();
            else if (type==ikv::Type::snapshot) {
                snapshot_source_=accepted_source_;
                ++snapshot_revision_; ++count_;
                if (!quit_) script_.run(count_,*session_,[&](const std::string& name){ if (scripted_effects_.size()<64) scripted_effects_.push_back(name); },
                                        // A scripted click takes the same path as a real left click: HUD first, then the world.
                                        [&](float x,float y){ if (!hud_click(x,y) && scene_.source_registry_id && original_ui_cached_) pick(x,y); });
                if (options_.ticks && count_>=options_.ticks && !quit_) leave();
            }
        });
    }
    void load_progress() {
        try { pump_network(); }
        catch (const std::exception& e) { throw ProgressConnectionError(e.what()); }
    }
    template<class Work> auto load_cpu(Work&& work) {
        // std::async's owning future joins even when transport progress throws.
        auto result=std::async(std::launch::async,std::forward<Work>(work));
        while (result.wait_for(std::chrono::milliseconds(10))!=std::future_status::ready) { load_progress(); if (loading_) paint_loading(); }
        return result.get();
    }
    bool pending_presentation() const noexcept {
        return state_pending_ || !pending_mutations_.empty() || snapshot_revision_!=painted_snapshot_revision_;
    }
    void apply_pending() {
        while (!pending_mutations_.empty()) {
            auto mutation=std::move(pending_mutations_.front());
            pending_mutations_.pop_front(); pending_bytes_-=mutation.bytes;
            const bool loading=!scene_.source_registry_id || *scene_.source_registry_id!=mutation.source;
            if (loading) begin_loading();
            load_authoritative_scene(mutation.source);
            attach_models(mutation,loading);
            if (loading) { loading_stage(1); loading_=false; } // the first real frame's GPU upload keeps this frame presented
            for (std::size_t i=0;i<mutation.actors.size();++i) {
                const auto& row=*mutation.actors[i];
                const auto id=row.at("id").get<std::uint32_t>();
                actor_life(id,row.at("hp").get<int>(),row.at("alive").get<bool>());
                move_actor({id,row.at("x").get<std::int32_t>(),row.at("y").get<std::int32_t>()},mutation.ticks[i]);
            }
            handle_events(mutation.events);
        }
        poll_assembly();
        const bool changed_state=state_pending_;
        if (changed_state) { state_=session_->state; state_pending_=false; }
        bool changed_snapshot=false;
        if (snapshot_source_!=scene_.source_registry_id ||
            (!state_.empty() && session_->tick<state_.at("tick").get<std::uint32_t>())) {
            players_.clear();
            painted_snapshot_revision_=snapshot_revision_;
        } else if (snapshot_revision_!=painted_snapshot_revision_) {
            players_=session_->players;
            painted_snapshot_revision_=snapshot_revision_; has_snapshot_=session_->has_snapshot;
            changed_snapshot=true;
        }
        if (changed_state) state_changed();
        if (changed_snapshot) movement_changed();
    }
    void shutdown() noexcept {
        session_.reset(); audio_.reset(); ui_.reset(); renderer_.reset();
        for (auto*& cursor:cursors_) if (cursor) { SDL_DestroyCursor(cursor); cursor=nullptr; }
        if (window_) { SDL_DestroyWindow(window_); window_=nullptr; }
        SDL_Quit();
    }
    std::filesystem::path asset_base() const {
        auto root=options_.assets;
        if (std::filesystem::is_directory(root/"app"/"base")) root/="app/base";
        else if (std::filesystem::is_directory(root/"base")) root/="base";
        return root;
    }
    void reset_renderer() {
        const auto camera=renderer_?renderer_->camera():options_.camera;
        renderer_.reset();
        ikv::RendererOptions ro; ro.camera=camera; ro.original_appearance=options_.appearance=="original";
        ro.ray_tracing=options_.rt=="required"?ikv::RayTracing::required:options_.rt=="auto"?ikv::RayTracing::automatic:ikv::RayTracing::off;
        ro.rt_style=options_.rt_style=="physical"?ikv::RtStyle::physical:ikv::RtStyle::faithful;
        ro.render_scale=static_cast<float>(options_.render_scale);
        ro.upscaler=options_.upscaler=="fsr3"?ikv::Upscaler::fsr3:ikv::Upscaler::bilinear;
        ro.fsr_quality=fsr_quality(options_.fsr_mode); ro.fsr_sharpness=static_cast<float>(options_.fsr_sharpness);
        ro.frame_generation=options_.frame_generation;
        ro.progress=[this]{load_progress();};
#if IKV_HAS_VULKAN
        renderer_=options_.renderer=="opengl"?ikv::make_gl_renderer(window_,ro):ikv::make_vk_renderer(window_,ro);
#else
        renderer_=ikv::make_gl_renderer(window_,ro);
#endif
    }
    // Original charsel/chargen module (docs/PORTING_GAPS.md #12, #13) when the launcher passes --auth instead of --character.
    bool choose_character() {
        ikv::CharacterApi api(options_.auth,options_.ca,secret_.view());
        begin_loading();
        auto scene=load_cpu([&]{return ikv::load_charsel_scene(options_.assets);});
        apply_view(scene);
        loading_=false;
        const ikv::CharacterScreenArt art{original_ui1_,loading_art_.logo,asset_base()/"fonts/verdana.ttf"};
        const auto chosen=ikv::run_character_screen(window_,*renderer_,*audio_,effects_.get(),scene,options_.assets,art,api,
                                                    []{return interrupted!=0;});
        // Providers borrow the charsel Scene: restore the world camera and rebuild before it is destroyed.
        renderer_->set_camera(options_.camera); reset_renderer();
        if (!chosen) { quit_=true; return false; }
        options_.character=*chosen; return true;
    }
    void load_authoritative_scene(unsigned source) {
        if (scene_.source_registry_id && *scene_.source_registry_id==source) return;
        auto replacement=load_cpu([&]{return ikv::load_source_scene(options_.assets,source);});
        // The record fog lives in the game state (004b43a0), not the zone: a received server fog survives the zone load.
        if (scene_.lighting.source_hour>=0) { replacement.lighting.fog_record=scene_.lighting.fog_record; replacement.lighting.fog_color=scene_.lighting.fog_color; }
        loading_stage(.5f);
        // Providers borrow Scene resources. Destroy them before replacing the source scene at this stable address.
        // Keep yaw/pitch; options_.camera.distance stays the desired zoom, the renderer holds the occlusion-clamped one.
        const auto camera=renderer_->camera();
        renderer_.reset(); options_.camera.yaw=camera.yaw; options_.camera.pitch=camera.pitch;
        ui_.reset(); scene_=std::move(replacement); actors_.clear(); poses_.clear(); sacks_.clear();
        entities_.clear(); rendered_.clear(); players_.clear();
        target_=0; help_page_=0; help_book_.clear(); elapsed_=0;
        reset_renderer(); init_original_ui(); loading_stage(.7f);
        effect_cues_.clear();
        if (effects_) effects_->attach_scene(scene_); // zone *_particles.adf type-11 emitters
        apply_prefs();
        // Scene diagnostics are developer evidence, not player HUD text.
        for (const auto& message:scene_.diagnostics) std::cerr << message << '\n';
        std::cerr << "Özgün sahne: " << ikv::source_scene_descriptor(source).label << '\n';
    }
    static ikv::assets::DdsImage read_ui_image(const std::filesystem::path& path) {
        const auto size=std::filesystem::file_size(path);
        if (size<128 || size>16*1024*1024) throw std::runtime_error("original UI DDS exceeds size limit");
        std::ifstream file(path,std::ios::binary);
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
        if (!file.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(size)) ||
            file.peek()!=std::char_traits<char>::eof())
            throw std::runtime_error("original UI DDS changed or read failed");
        const auto header=ikv::assets::read_dds_header(bytes,bytes.size());
        if (header.width>4096 || header.height>4096 ||
            static_cast<std::uint64_t>(header.width)*header.height>4*1024*1024)
            throw std::runtime_error("original UI DDS decoded dimensions exceed limit");
        return ikv::assets::decode_dds(bytes);
    }
    void init_original_ui() {
        ui_px_=0; // draw() recreates the resolution-scaled UI font for the new Scene.font.
        if (!original_ui_cached_) {
            struct Images {
                ikv::assets::DdsImage first,second;
                std::vector<std::pair<std::string,ikv::assets::DdsImage>> help;
                std::vector<ikv::SourceSkill> skills;
                std::map<int,SourcePrefix> prefixes;
            };
            const auto base=asset_base();
            auto loaded=load_cpu([&] {
                Images images;
                images.first=read_ui_image(base/"ui/userinterface.dds");
                images.second=read_ui_image(base/"ui/userinterface2.dds");
                std::size_t cached_bytes=images.first.rgba.size()+images.second.rgba.size();
                // Real filename keys are selected independently of helps.xml page headers.
                for (const auto* key:{"CharacterSelection","Userinterface","Movement","Battle","Trade","Bank","Quest","Clan","Chat"}) {
                    const auto path=base/"ui"/(std::string("Help_")+key+".dds");
                    if (!std::filesystem::is_regular_file(path)) continue;
                    auto image=read_ui_image(path);
                    cached_bytes+=image.rgba.size();
                    if (cached_bytes>128*1024*1024) throw std::runtime_error("original UI cache exceeds byte limit");
                    images.help.emplace_back(key,std::move(image));
                }
                images.skills=ikv::load_source_skills(base);
                for (const auto& r:ikv::load_source_records(base,"prefixes.dat")) {
                    const auto at=[&](const char* key){const auto f=r.attributes.find(key);return f==r.attributes.end()?std::string{}:f->second;};
                    SourcePrefix p{at("name"),at("effect_name"),{}};
                    std::istringstream effects(at("attreffects")); std::string attribute,value;
                    while (std::getline(effects,attribute,';') && std::getline(effects,value,';')) p.attributes.push_back({std::atoi(attribute.c_str()),std::atof(value.c_str())});
                    images.prefixes[std::atoi(at("uniqueid").c_str())]=std::move(p);
                }
                return images;
            });
            original_ui1_=std::move(loaded.first); original_ui2_=std::move(loaded.second);
            help_images_=std::move(loaded.help); source_skills_=std::move(loaded.skills); source_prefixes=std::move(loaded.prefixes);
            original_ui_cached_=true;
        }
        hud_dirty_=true;
    }
    static std::array<std::uint8_t,4> original_rgba(std::uint32_t argb) {
        return {static_cast<std::uint8_t>(argb>>16),static_cast<std::uint8_t>(argb>>8),
                static_cast<std::uint8_t>(argb),static_cast<std::uint8_t>(argb>>24)};
    }
    void atlas_image(const ikv::assets::DdsImage& image,float x,float y,float w,float h,
                     float u,float v,float sw,float sh,std::uint32_t argb=0xffffffff) {
        const float left=u/1024.5f,right=(u+sw)/1024.5f,top=v/1023.5f,bottom=(v+sh)/1023.5f;
        ui_->image(image,{x,y,w,h},{{{left,top},{right,top},{left,bottom},{right,bottom}}},original_rgba(argb));
    }
    static float resource_ratio(const Json& state,const char* current,const char* maximum) {
        const float cap=state.value(maximum,0.f);
        return cap>0?std::clamp(state.value(current,0.f)/cap,0.f,1.f):0.f;
    }
    // ---- Original-style HUD. Layout is in original 1080p atlas pixels scaled by s_. ----
    using Rgba=std::array<std::uint8_t,4>;
    static constexpr Rgba text_rgba{232,226,206,255},gold_rgba{238,205,129,255},dim_rgba{165,165,155,255},white_rgba{255,255,255,255};
    // Quest text (00448dd0): section headers ARGB ffaaff00; the server-coloured title line reads yellow in the VOD.
    static constexpr Rgba quest_title_rgba{255,255,0,255},quest_header_rgba{170,255,0,255};
    // Original info windows (Karakter Bilgisi, Görevler, Görev al) use a ~10 px@1080 face (VOD cap height ~7.5 px), not the 16 px HUD text.
    int small_px() const { return std::max(7,static_cast<int>(std::lround(10*s_))); }
    template<class... A> static std::string printf_text(const char* pattern,A... args) { char text[160]; std::snprintf(text,sizeof(text),pattern,args...); return text; }
    struct Flow { int left,right,x,y; };
    int S(float v) const { return static_cast<int>(std::lround(v*s_)); }
    int lh() const { return ui_->line_height(); }
    void ui_atlas(const ikv::assets::DdsImage& image,float x,float y,float dx,float dy,float dw,float dh,
                  float u,float v,float sw,float sh,std::uint32_t argb=0xffffffff) {
        atlas_image(image,x+dx*s_,y+dy*s_,dw*s_,dh*s_,u,v,sw,sh,argb);
    }
    void label(int x,int y,std::string_view text,Rgba color=text_rgba) {
        ui_->text(x+1,y+1,text,{0,0,0,color[3]}); ui_->text(x,y,text,color);
    }
    // Wraps at the last space when possible, otherwise between UTF-8 scalars.
    std::vector<std::string> wrap(const std::string& text,int width) {
        std::vector<std::string> lines; std::string line;
        for (std::size_t i=0;i<text.size();) {
            const auto first=static_cast<unsigned char>(text[i]);
            const std::size_t count=first<0x80?1:first<0xe0?2:first<0xf0?3:4;
            const auto length=std::min(count,text.size()-i),before=line.size();
            line.append(text,i,length); i+=length;
            if (before && ui_->text_width(line)>width) {
                const auto space=line.rfind(' ',before-1);
                const auto cut=space!=std::string::npos && space>0?space:before;
                auto rest=line.substr(cut+(cut==space?1:0)); line.resize(cut);
                lines.push_back(std::move(line)); line=std::move(rest);
            }
        }
        if (!line.empty()) lines.push_back(std::move(line));
        return lines;
    }
    void centered(int cx,int cy,std::string_view text,Rgba color=text_rgba) { label(cx-ui_->text_width(text)/2,cy-lh()/2,text,color); }
    void percent(float cx,float cy,float ratio) {
        centered(static_cast<int>(cx),static_cast<int>(cy),std::to_string(std::lround(ratio*100))+"%");
    }
    void outline(int x,int y,int w,int h,Rgba color) {
        ui_->rect(x,y,w,1,color); ui_->rect(x,y+h-1,w,1,color); ui_->rect(x,y,1,h,color); ui_->rect(x+w-1,y,1,h,color);
    }
    static void pop_utf8(std::string& text) {
        do { const auto c=static_cast<unsigned char>(text.back()); text.pop_back(); if ((c&0xc0)!=0x80) break; } while (!text.empty());
    }
    std::string fit(std::string text,int width) {
        if (ui_->text_width(text)<=width) return text;
        while (!text.empty() && ui_->text_width(text+"…")>width) pop_utf8(text);
        return text+"…";
    }
    // Word wrap with explicit newlines; returns the next free y.
    int paragraph(int x,int y,int width,int bottom,std::string_view text,Rgba color=text_rgba) {
        std::string line;
        auto flush=[&]{ if (y+lh()<=bottom) ui_->text(x,y,line,color); y+=lh(); line.clear(); };
        while (!text.empty() && y+lh()<=bottom) {
            const auto split=text.find_first_of(" \n\t\r");
            const auto word=text.substr(0,split);
            const auto before=line.size();
            if (!line.empty()) line+=' ';
            line.append(word);
            if (before && ui_->text_width(line)>width) { line.resize(before); flush(); line.assign(word); }
            if (split==std::string_view::npos) break;
            const bool newline=text[split]=='\n';
            text.remove_prefix(split+1);
            if (newline) flush();
        }
        if (!line.empty()) flush();
        return y;
    }
    int button(int x,int y,const std::string& text,Json action=nullptr,int mode=-1,std::string value={},int w=0) {
        const int h=lh()+S(6);
        if (!w) w=ui_->text_width(text)+S(16);
        ui_->rect(x,y,w,h,{96,84,62,235}); ui_->rect(x+1,y+1,w-2,h-2,{20,24,28,235});
        const auto shown=fit(text,w-4);
        ui_->text(x+(w-ui_->text_width(shown))/2,y+(h-lh())/2,shown,text_rgba);
        hits_.push_back({x,y,w,h,std::move(action),mode,std::move(value)});
        return w;
    }
    void flow(Flow& f,const std::string& text,Json action=nullptr,int mode=-1,std::string value={}) {
        const int w=std::min(ui_->text_width(text)+S(16),f.right-f.left);
        if (f.x>f.left && f.x+w>f.right) { f.x=f.left; f.y+=lh()+S(12); }
        button(f.x,f.y,text,std::move(action),mode,std::move(value),w); f.x+=w+S(6);
    }
    int flow_end(const Flow& f) const { return f.x>f.left?f.y+lh()+S(12):f.y; }
    void center_button(int y,const std::string& text,Json action,std::string value={}) {
        const int w=ui_->text_width(text)+S(40);
        button((width_-w)/2,y,text,std::move(action),-1,std::move(value),w);
    }
    bool is_open(int id) const { return std::find(windows_.begin(),windows_.end(),id)!=windows_.end(); }
    void toggle_window(int id,bool keep_open=false) {
        const auto found=std::find(windows_.begin(),windows_.end(),id);
        // 0042e970: original window toggles play ui\windowclose.raw / ui\windowopen.raw.
        if (found!=windows_.end()) { if (!keep_open) { windows_.erase(found); audio_->play_event("ui\\windowclose.raw"); } return; }
        windows_.push_back(id); scroll_[static_cast<std::size_t>(id)]=0;
        audio_->play_event("ui\\windowopen.raw");
    }
    const Json* entity(std::uint32_t id) const {
        if (session_ && state_.contains("entities")) for (const auto& e:state_.at("entities"))
            if (e.value("id",0u)==id) return &e;
        return nullptr;
    }
    // Original UI2 plain window (close x top-right), nine-sliced so the frame keeps atlas pixels.
    // Returns the content rectangle as left, top, right, bottom.
    std::array<int,4> frame(int id,int x,int y,int w,int h,const std::string& title) {
        const float b=28,u=806,v=700,sw=209,sh=312,d=b*s_;
        const float fx=static_cast<float>(x),fy=static_cast<float>(y),fw=static_cast<float>(w),fh=static_cast<float>(h);
        const std::array<float,4> xs{fx,fx+d,fx+fw-d,fx+fw},ys{fy,fy+d,fy+fh-d,fy+fh};
        const std::array<float,4> us{u,u+b,u+sw-b,u+sw},vs{v,v+b,v+sh-b,v+sh};
        for (std::size_t i=0;i<3;++i) for (std::size_t j=0;j<3;++j)
            atlas_image(original_ui2_,xs[i],ys[j],xs[i+1]-xs[i],ys[j+1]-ys[j],us[i],vs[j],us[i+1]-us[i],vs[j+1]-vs[j]);
        hits_.push_back({x,y,w,h,nullptr,-1,{}});
        if (id) { window_rects_.push_back({id,{x,y,w,h}}); hits_.push_back({x+w-S(30),y,S(30),S(28),nullptr,id,{}}); }
        label(x+S(16),y+S(14)-lh()/2,fit(title,w-S(50)),gold_rgba);
        return {x+S(24),y+S(34),x+w-S(24),y+h-S(14)};
    }
    // Player frame (UI2 0,0) with party and target frames, as in the original top-left corner.
    void paint_frames(const Json& st,const std::string& name) {
        const float hp=resource_ratio(st,"hp","max_hp"),mana=resource_ratio(st,"mana","max_mana");
        const float interval=session_?state_.value("xp_interval",0.f):0.f;
        const float xp=interval>0?std::clamp(st.value("xp",0.f)/interval,0.f,1.f):0.f;
        ui_atlas(original_ui2_,0,0,0,0,166,111,0,0,166,111);
        // Independent mana is the explicit adapter for the original Kudret display.
        const std::array<std::pair<float,float>,3> bars{{{hp,153},{mana,156},{xp,159}}};
        for (std::size_t i=0;i<bars.size();++i) {
            const float y=29+18*static_cast<float>(i);
            ui_atlas(original_ui2_,0,0,16,y,138*bars[i].first,8,bars[i].second,189,2,8);
            ui_atlas(original_ui2_,0,0,13,y-3,143,11,168,39,143,11);
            percent(85*s_,(y+4)*s_,bars[i].first);
        }
        const std::string level=" ("+std::to_string(st.value("level",0))+")";
        label(S(15),S(15.5f)-lh()/2,fit(name,S(92)-ui_->text_width(level))+level);
        if (session_) hits_.push_back({0,0,S(166),S(100),nullptr,-1,"target:"+std::to_string(session_->self)});
        paint_effects(st.value("buffs",Json::array()),168,5,25); // 0x486370: own effects beside the frame, at most 25
        // NONCANONICAL Er Meydanı area bonus (stats.arena_power: skill amounts +% while the own clan holds capture areas) as a
        // buff-like tag under the own effect row; no original indicator exists.
        if (const int power=state_.value("stats",Json::object()).value("arena_power",0)) {
            const auto rows=static_cast<float>((std::min<std::size_t>(st.value("buffs",Json::array()).size(),25)+4)/5);
            label(S(168),S(std::max(78.f,7+17*rows)),printf_text("Meydan gücü +%%%d",power),gold_rgba); // below a target frame (≤ 74 high)
        }
        float y=112; // party frames at 0,112+74·slot (0x4845a0)
        for (const auto& member:group_members_) {
            if (!session_ || !member.is_number_unsigned() || member==session_->self) continue;
            const auto id=member.get<std::uint32_t>();
            const Json* e=entity(id);
            ui_atlas(original_ui2_,0,0,0,y,168,74,0,112,168,74);
            if (e) {
                paint_effects(e->value("debuffs",Json::array()),168,y+5,15);
                ui_atlas(original_ui2_,0,0,16,y+27,59*resource_ratio(*e,"hp","max_hp"),8,153,189,2,8);
                ui_atlas(original_ui2_,0,0,14,y+24,62,11,169,51,62,11);
                percent(45.5f*s_,(y+31)*s_,resource_ratio(*e,"hp","max_hp"));
            }
            label(S(15),S(y+15)-lh()/2,fit(e?e->value("name",std::string{}):"#"+std::to_string(id),S(92)));
            hits_.push_back({0,S(y),S(168),S(60),nullptr,-1,"target:"+std::to_string(id)});
            y+=74;
            if (y>330) break;
        }
        Json self_row;
        const Json* subject=entity(target_);
        if (!subject && session_ && target_==session_->self && target_) {
            self_row=Json{{"name",name},{"kind","player"},{"level",st.value("level",0)},{"hp",st.value("hp",0)},{"max_hp",st.value("max_hp",0)},
                {"mana",st.value("mana",0)},{"max_mana",st.value("max_mana",0)},{"debuffs",st.value("buffs",Json::array())}};
            subject=&self_row;
        }
        if (!subject) { context_=0; return; }
        const float x=300; // PC/NPC target frames at 300,0 (0x485110 / 0x485bf0)
        const bool player=subject->value("kind",std::string{})=="player";
        const float target_hp=resource_ratio(*subject,"hp","max_hp");
        if (player) {
            ui_atlas(original_ui2_,0,0,x,0,168,74,0,112,168,74);
            ui_atlas(original_ui2_,0,0,x+16,27,59*target_hp,8,153,189,2,8);
            ui_atlas(original_ui2_,0,0,x+14,24,62,11,169,51,62,11);
            percent((x+45.5f)*s_,31*s_,target_hp);
            if (subject->contains("mana") && subject->contains("max_mana")) {
                const float target_mana=resource_ratio(*subject,"mana","max_mana");
                ui_atlas(original_ui2_,0,0,x+95,27,59*target_mana,8,156,189,2,8);
                percent((x+124.5f)*s_,31*s_,target_mana);
            }
            ui_atlas(original_ui2_,0,0,x+93,24,62,11,169,51,62,11);
        } else {
            ui_atlas(original_ui2_,0,0,x,0,163,54,4,559,163,54);
            ui_atlas(original_ui2_,0,0,x+14,22,136*target_hp,8,153,189,2,8);
            percent((x+82)*s_,26*s_,target_hp);
        }
        const std::string suffix=subject->contains("level")?" ("+std::to_string(subject->value("level",0))+")":"";
        label(S(x+14),S(player?15.f:9.f)-lh()/2,fit(subject->value("name",std::to_string(target_)),S(92)-ui_->text_width(suffix))+suffix);
        hits_.push_back({S(x),0,S(168),S(player?74.f:54.f),nullptr,-1,"context"});
        paint_effects(subject->value("debuffs",Json::array()),x+168,5,15); // the target's effects beside its frame, at most 15
        float menu_y=player?76.f:56.f;
        if (!context_) return;
        // Contextual actions for the selected target; the server validates every request.
        std::vector<std::tuple<std::string,Json,std::string>> items;
        const auto kind=subject->value("kind",std::string{});
        // 0041a230 offers nothing on a hostile (00478090 == 1) or a <loc>: attacking and looting are clicks on the body or the Ganimet sack.
        if (player) {
            if (session_ && target_==session_->self) { // 0041a230 self: PRVL Gruptan Çık in a group, else MDBK Kabadayı Moduna Geç; BANK; privileged SNAY
                if (!group_members_.empty()) items.emplace_back("Gruptan Çık",nullptr,"group-leave");
                else if (!st.value("kabadayi",false)) items.emplace_back("Kabadayı Moduna Geç",nullptr,"kabadayi");
                items.emplace_back("Kasamı Göster",nullptr,"bank-open"); // 0x58ec64 'BANK', always listed; the server checks Kasa Erişimi (guide l.875-878)
                if (state_.value("admin",false)) items.emplace_back("İsim Yansıt",nullptr,"reflect:"+std::to_string(target_));
            } else player_items(items,target_,false);
        } else if (kind!="enemy" && relation(*subject)!=1) { // 0041a230 NPC branch: TALK (+0x80&0x50), BANK (+0x81&0x10), DART traders, clan items
            if (kind=="node") items.emplace_back("Topla",target_action("gather"),""); // Toplama node (classes.dat <loc> maden*)
            else if (kind=="anvil") items.emplace_back("Tamir et",target_action("repair"),""); // örs (helps.xml Para: sağ tık ile tamir)
            else if (kind=="reset" || (kind=="prop" && subject->value("interactable",false))) items.emplace_back("Kullan",target_action("interact"),""); // GBM-0 / GBM 0-B (guide: sağ tuşla etkileşerek), <loc> quest props; label NONCANONICAL
            else if (kind!="door" && kind!="summon" && kind!="trap" && kind!="prop") items.emplace_back("Konuş",target_action("interact"),"");
            if (subject->value("banker",false)) items.emplace_back("Kasamı göster",nullptr,"bank-open");
            if (kind=="merchant") items.emplace_back("Alışveriş Başlat",nullptr,"vendor");
            if (subject->value("recruiter",false)) { // claninviter (flag 0x20000): Klana Katıl while clanless, Klandan Ayrıl on its own clan
                const int clan=subject->value("clan",0);
                if (!st.contains("clan")) items.emplace_back("Klana Katıl",target_action("clan_join"),"");
                else if (st.value("clan",0)==clan) items.emplace_back("Klandan Ayrıl",nullptr,"clan-leave");
            }
        }
        if (items.empty()) return;
        const int row=lh()+S(10),count=static_cast<int>(items.size());
        const auto content=frame(0,S(x),S(menu_y),S(190),S(48)+count*row,"İşlemler");
        int ry=content[1];
        for (auto& [text,action,value]:items) { button(content[0],ry,text,std::move(action),-1,value,content[2]-content[0]); ry+=row; }
    }
    // Original player menus: target 0041a230 and chat-line name 0040aad0. Items (command ids): SIHW Fısılda; VMER Gruptan Çıkar
    // for a member when leading, else PART Gruba Davet Et while ungrouped or leading; DLUG Loncaya Davet Et with a guild; DART
    // Alışveriş Başlat (target menu); DNRF/RNGI Arkadaşlara/Yasaklılara Ekle; PMOC Şikayet Et (chat menu); privileged (account
    // level %100 > 0 / > 5 in the original, administrators here) SNAY İsim Yansıt and NISI Yanına Işınla.
    void player_items(std::vector<std::tuple<std::string,Json,std::string>>& items,std::uint32_t id,bool chat) {
        const auto& own=character_state(state_);
        const auto act=[&](const char* action){return Json{{"action",action},{"target",id}};};
        const auto sid=std::to_string(id);
        const bool leading=session_ && group_leader_==session_->self;
        items.emplace_back("Fısılda",nullptr,"whisper:"+sid);
        if (!chat && leading && std::find(group_members_.begin(),group_members_.end(),Json(id))!=group_members_.end()) items.emplace_back("Gruptan Çıkar",nullptr,"group-kick:"+sid);
        else if (chat || group_members_.empty() || leading) items.emplace_back("Gruba Davet Et",act("group_invite"),"");
        if (own.contains("guild") && own.at("guild").is_object()) {
            items.emplace_back("Loncaya Davet Et",act("guild_invite"),"");
            if (const Json* e=entity(id); !chat && e && e->value("guild",std::string{})==own.at("guild").value("name",std::string{}))
                items.emplace_back("Loncadan çıkar",act("guild_kick"),""); // native: no original menu item for guild removal
        }
        if (!chat) items.emplace_back("Alışveriş Başlat",nullptr,"trade");
        items.emplace_back("Arkadaşlara Ekle",Json{{"action","contact_add"},{"list","friends"},{"target",id}},"");
        items.emplace_back("Yasaklılara Ekle",Json{{"action","contact_add"},{"list","ignored"},{"target",id}},"");
        if (chat) items.emplace_back("Şikayet Et",act("complain"),"");
        if (state_.value("admin",false)) {
            items.emplace_back("İsim Yansıt",nullptr,"reflect:"+sid);
            items.emplace_back("Yanına Işınla",act("teleport_here"),"");
        }
    }
    // Chat-line name for chat menus, otherwise the visible entity name.
    std::string name_of(std::uint32_t id) const {
        if (id==chat_menu_id_ && !chat_menu_name_.empty()) return chat_menu_name_;
        const Json* e=entity(id);
        return e?e->value("name",std::string{}):"#"+std::to_string(id);
    }
    std::optional<std::array<int,2>> project(float x,float y,float z) const {
        const auto m=ikv::camera_view_projection(renderer_->camera(),width_,height_);
        const float cx=m[0]*x+m[4]*y+m[8]*z+m[12],cy=m[1]*x+m[5]*y+m[9]*z+m[13],cw=m[3]*x+m[7]*y+m[11]*z+m[15];
        if (cw<.1f) return {};
        return std::array<int,2>{static_cast<int>(std::lround((cx/cw+1)*.5f*static_cast<float>(width_))),
                                 static_cast<int>(std::lround((1-cy/cw)*.5f*static_cast<float>(height_)))};
    }
    // 0047c530: the root position + 7.0 up through 0046be50, which projects with D3DXVec3Project and a near plane of 0.1
    // and returns (sx·sz, sy·sz, sz) — the screen point scaled by its [0,1] depth, kept as the original draws it.
    std::optional<std::array<float,3>> tag_anchor(const ikv::RenderEntity& p) const {
        const auto& c=renderer_->camera();
        const auto m=ikv::camera_view_projection(c,width_,height_);
        const float x=p.x,y=p.ground_y+7.f,z=p.z;
        const float cx=m[0]*x+m[4]*y+m[8]*z+m[12],cy=m[1]*x+m[5]*y+m[9]*z+m[13],cw=m[3]*x+m[7]*y+m[11]*z+m[15];
        if (cw<.1f) return {};
        const float sz=c.far_plane/(c.far_plane-.1f)*(1-.1f/cw);
        return std::array<float,3>{(cx/cw+1)*.5f*static_cast<float>(width_)*sz,(1-cy/cw)*.5f*static_cast<float>(height_)*sz,sz};
    }
    // 0047c820: root +0xf4 = the classes.dat half-extent box, centred on the root position (no +0130 offset, unlike the
    // pick probe 0047cb80) and turned with the root, intersects the view frustum (0x462750).
    // ponytail: tested every frame; the original re-tests visible roots every 20th frame and hidden ones every 2nd/4th.
    bool root_visible(const ikv::FrustumPlanes& planes,const ikv::RenderEntity& p,const std::array<float,3>& b) const {
        const float s=std::sin(p.yaw),c=std::cos(p.yaw);
        for (const auto& n:planes) {
            const float r=b[0]*std::abs(n[0]*c-n[2]*s)+b[1]*std::abs(n[1])+b[2]*std::abs(n[0]*s+n[2]*c);
            if (n[0]*p.x+n[1]*p.ground_y+n[2]*p.z+n[3]<-r) return false;
        }
        return true;
    }
    // Original 00476eb0 skins an actor only while its root's frustum flag (+0xf4, root_visible above) is set; held actors
    // keep their clip clocks and bones (SceneActorAssembly::skin_held). Here the box is padded (swings and falls leave
    // the classes.dat box) and swept along the sun shadow, which RT casts into view from outside it. Uses this frame's
    // poses and camera; returns whether an actor held off screen came into view, whose skin must be redone before render.
    bool hold_offscreen_skins() {
        bool released=false;
        if (!renderer_ || width_<=0 || height_<=0) return released;
        const auto planes=ikv::frustum_planes(ikv::camera_view_projection(renderer_->camera(),width_,height_));
        const auto& light=scene_.lighting;
        const auto sun=light.source_values?light.sun_direction:std::array<float,3>{.48f,.78f,.4f};
        // The shadow of the box top lands h·(sun.xz/sun.y) away from the sun; low suns are capped at 80 units.
        const float horizontal=std::hypot(sun[0],sun[2]),run=sun[1]>0?horizontal/sun[1]:1e9f;
        for (auto& assembly:scene_.actor_assemblies) {
            auto* actor=actor_state(assembly.entity_id);
            const auto* p=actor?pose(assembly.entity_id):nullptr;
            bool visible=true; // no pose: not drawn this frame, keep its skin current for when it is
            if (p && !(session_ && p->id==session_->self)) {
                const float h=actor->height,rx=std::max({actor->bounds[0],actor->bounds[2],1.5f})*1.5f+1,ry=h*.75f+1;
                const float cx=p->x,cy=p->ground_y+h*.5f,cz=p->z;
                const float length=std::min(h*run,80.f),dx=horizontal>0?-sun[0]/horizontal*length:0,dz=horizontal>0?-sun[2]/horizontal*length:0;
                for (const auto& n:planes) {
                    const float r=rx*(std::abs(n[0])+std::abs(n[2]))+ry*std::abs(n[1]);
                    const float d=n[0]*cx+n[1]*cy+n[2]*cz+n[3];
                    if (d<-r && d+n[0]*dx-n[1]*h*.5f+n[2]*dz<-r) { visible=false; break; }
                }
            }
            if (actor) {
                released|=visible && actor->skin_offscreen && assembly.skin_held;
                actor->skin_offscreen=!visible;
            }
            assembly.skin_held=!visible;
        }
        return released;
    }
    // Project visible actors for body picking and name tags; repaint the overlay only when a tag moves.
    // Original name tags (0046fdb0 loop 00471a00..00471c9b): only for another PC (classid +0x1a4 < 8, entity type
    // flags & 0xf == 1) whose root passed the frustum visibility test (+0xf4, 0047cf50/0047c820; the box occlusion
    // test 004754d4 is a debug toggle, off by default, so names show through walls as in VOD 10:00) and whose
    // distance from the local player (00478150) is under 170 (0x595f8c). NPCs, monsters and the own character get
    // no in-world name (VOD 10:00, 16:30, 25:00): they are targeted by body pick, cursor and the target frame.
    // The loop's other gate, byte 0x610d07 (also gating the stat floaters at 004a5e45), has no writer anywhere in the
    // image (no direct, indexed or pointer reference besides the two reads), so it stays 0 and is not ported.
    void update_labels() {
        std::vector<Label> next; next.reserve(poses_.size());
        const auto* self=session_?pose(session_->self):nullptr;
        if (self && width_>0 && height_>0) {
            const auto planes=ikv::frustum_planes(ikv::camera_view_projection(renderer_->camera(),width_,height_));
            for (const auto& pose:poses_) {
                if (pose.id==session_->self) continue;
                const auto* actor=actor_state(pose.id);
                if (!root_visible(planes,pose,actor?actor->bounds:std::array<float,3>{})) continue;
                const auto head=project(pose.x,pose.ground_y+actor_height(pose.id),pose.z),feet=project(pose.x,pose.ground_y,pose.z);
                const auto tag=tag_anchor(pose);
                if (!head || !feet || !tag) continue;
                const Json* e=entity(pose.id);
                const float dx=pose.x-self->x,dy=pose.ground_y-self->ground_y,dz=pose.z-self->z;
                const bool name=e && e->value("kind",std::string{})=="player" && dx*dx+dy*dy+dz*dz<170.f*170.f;
                // 00471dfa: lift = boundy (root +0x128) · 0.3 · (1 − sz)·2997; the name line's top is 14 px above the lift.
                const float lift=(actor?actor->bounds[1]:0.f)*.3f*(1-(*tag)[2])*2997.f;
                next.push_back({pose.id,(*head)[0],(*head)[1],(*feet)[1],static_cast<int>((*tag)[0]),static_cast<int>((*tag)[1]-lift*s_),name});
            }
        }
        // paint_labels draws only named entries (by id and tag); unnamed ones serve label_at() hit tests alone, so their
        // per-frame motion must not repaint the whole HUD.
        auto a=labels_.cbegin(),b=next.cbegin();
        for (;;++a,++b) {
            while (a!=labels_.cend() && !a->name) ++a;
            while (b!=next.cend() && !b->name) ++b;
            if (a==labels_.cend() || b==next.cend()) { if (a!=labels_.cend() || b!=next.cend()) world_dirty_=true; break; }
            if (a->id!=b->id || a->tx!=b->tx || a->ty!=b->ty) { world_dirty_=true; break; }
        }
        labels_=std::move(next);
    }
    std::uint32_t label_at(float x,float y) const {
        std::uint32_t best=0; float distance=static_cast<float>(S(40));
        for (const auto& l:labels_) {
            const float dx=std::abs(x-static_cast<float>(l.x));
            if (dx<distance && y>=static_cast<float>(l.head-S(20)) && y<=static_cast<float>(l.feet)) { distance=dx; best=l.id; }
        }
        return best;
    }
    // Original 00478090 seen from the local player: 1 hostile, 2 neutral, 3 friendly. Its clan test (00478050) applies to every
    // entity: different nonzero clans are hostile (PCs and Klan OOK guards alike). PCs: two Kabadayı are hostile, one Kabadayı
    // neutral, then clan membership; NPCs: enemies hostile, own-clan NPCs friendly, other NPCs neutral (VOD yellow ring).
    int relation(const Json& e) const {
        const auto& own=character_state(state_);
        const int clan=own.value("clan",0),other=e.value("clan",0);
        if (e.value("kind",std::string{})=="summon") return 3; // Boz Ayı/Tesla Küresi/Çağrı fight for a player: friendly (green)
        if (clan && other && clan!=other) return 1;
        if (e.value("kind",std::string{})!="player") return clan && other==clan?3:e.value("kind",std::string{})=="enemy"?1:2; // own banners/guards friendly
        const bool mine=own.value("kabadayi",false),theirs=e.value("kabadayi",false);
        if (mine && theirs) return 1;
        if (mine || theirs) return 2;
        return clan==other?3:2;
    }
    // Character window clan table (0x58da7c "Yok", 0x58da6c "Arzın Çocukları", 0x58da64 "Lodos"; entity flag bits 8-11).
    static const char* clan_name(int clan) { static constexpr std::array<const char*,3> names{"Yok","Arzın Çocukları","Lodos"}; return clan>=0 && clan<3?names[static_cast<std::size_t>(clan)]:"?"; }
    float& setting(int i) { return i==0?options_.music_volume:i==1?options_.volume:options_.prefs[static_cast<std::size_t>(i-2)]; }
    // Original 0041c07d applies Ayarlar: Görüş Mesafesi scales the fog distances (0046b630/0046b660, floor .1),
    // Ortam/Karakter Efektleri bound emitters to pref·160000 squared units (00478586). Karakter Detay Mes. and Ölü Yok
    // Olma Süresi are read per frame (default outfit beyond pref·170 in queue_state; corpse hide in the body loop).
    void apply_prefs() {
        audio_->mixer.volume(options_.volume); audio_->music_volume(options_.music_volume);
        apply_view(scene_);
    }
    // 00478586 also hides every emitter beyond sqrt(.5·k)·fog far (0x77c320 = far²).
    void apply_view(ikv::Scene& scene) {
        ikv::set_scene_fog(scene.lighting,std::max(.1f,options_.prefs[1]));
        if (effects_) effects_->set_ranges(options_.prefs[2]*160000,options_.prefs[3]*160000,scene.lighting.fog_far*scene.lighting.fog_far);
    }
    // Esc menu (00422890: 96×112 panel, buttons 80×24) and the "Dikkat" 400×64 confirm box (0x58f27c).
    void paint_system() {
        if (!confirm_.is_null()) {
            const bool split=confirm_.is_object() && confirm_.value("action",std::string{})=="split";
            // At least the original two-line box; long texts (sell names, GBM prices) and the split field grow it above Evet/Hayır.
            const int lines=static_cast<int>(wrap(confirm_text_,S(400)-S(48)).size());
            const auto content=frame(0,(width_-S(400))/2,height_/3,S(400),std::max(S(110)+lh(),S(64)+(lines+1)*lh()+(split?lh()+S(8):0)),"Dikkat");
            const int below=paragraph(content[0],content[1],content[2]-content[0],content[3],confirm_text_);
            if (split) { // the split box's number field
                ui_->rect(content[0],below+S(2),S(80),lh()+S(4),{8,8,12,230}); outline(content[0],below+S(2),S(80),lh()+S(4),{150,130,90,255});
                ui_->text(content[0]+S(4),below+S(4),split_amount_+"_",white_rgba);
            }
            Flow f{content[0],content[2],content[0],content[3]-lh()-S(8)};
            flow(f,"Evet",nullptr,-1,"confirm-yes"); flow(f,"Hayır",nullptr,-1,"confirm-no");
        } else if (system_menu_) {
            // 00422890: caption "Ana Menü" (0x58f2d4 set on the 96×112 window), visible buttons Oyuna Dön y18, Ayarlar y42,
            // Çıkış y66; its Yardım button is built hidden at 0,0 (flag 0), so F1 stays the only help entry.
            const int w=S(150),x=(width_-w)/2;
            const auto content=frame(0,x,height_/3,w,S(48)+3*(lh()+S(10)),"Ana Menü");
            int y=content[1];
            for (const auto& [text,value]:std::array<std::pair<const char*,const char*>,3>{{{"Oyuna Dön","system-close"},
                {"Ayarlar","system-settings"},{"Çıkış","system-quit"}}}) { button(content[0],y,text,nullptr,-1,value,content[2]-content[0]); y+=lh()+S(10); }
        }
    }
    void paint_labels() {
        for (const auto& l:labels_) {
            const Json* e=l.name?entity(l.id):nullptr;
            if (!e) continue;
            const auto name=e->value("name",std::string{});
            // 00471e41 (relation 00478090): hostile 0xffaa22dd, same clan 0xff33cccc, otherwise white. Rect left = x − width/2;
            // the name's top is 14 px above the lifted anchor and a Lonca member's "<%s>" (0x595f7c) line starts at it.
            const int clan=character_state(state_).value("clan",0);
            const Rgba color=original_rgba(relation(*e)==1?0xffaa22dd:clan && e->value("clan",0)==clan?0xff33cccc:0xffffffff);
            const int w=ui_->text_width(name);
            label(l.tx-w/2,l.ty-S(14),name,color);
            if (e->contains("guild")) {
                const auto guild="<"+e->value("guild",std::string{})+">";
                label(l.tx-ui_->text_width(guild)/2,l.ty,guild,color);
            }
            hits_.push_back({l.tx-w/2,l.ty-S(14),w,lh(),nullptr,-1,"target:"+std::to_string(l.id)});
        }
    }
    // Original UI1 "Mesajlaşma" frame and "Söyle:" input bar, three-sliced to the chosen height.
    void paint_chat() {
        const float h=200+40*static_cast<float>(chat_extra_);
        const float x=4*s_,y=static_cast<float>(height_)-(h+64)*s_;
        ui_atlas(original_ui1_,x,y,0,0,315,60,705,9,315,60);
        ui_atlas(original_ui1_,x,y,0,60,315,h-100,705,69,315,576);
        ui_atlas(original_ui1_,x,y,0,h-40,315,40,705,645,315,40);
        ui_atlas(original_ui1_,x,y,1,h,318,60,706,694,318,60);
        const int ix=static_cast<int>(x),iy=static_cast<int>(y);
        chat_rect_={ix,iy,S(319),S(h+60)};
        hits_.push_back({ix,iy,S(319),S(h+60),nullptr,-1,{}});
        auto hit=[&](float dx,float dy,float w,float hh,int mode,std::string value) {
            hits_.push_back({ix+S(dx),iy+S(dy),S(w),S(hh),nullptr,mode,std::move(value)});
        };
        label(ix+S(20),iy+S(16)-lh()/2,"Mesajlaşma");
        hit(17,9,96,15,-1,"channel");
        hit(177,4,59,22,win_contacts,{}); hit(240,6,17,18,-1,"help-book:Chat"); // '?' 0040e3f3 -> 00408dc0 opens the Chat book
        hit(258,6,17,18,-1,"chat-filter"); hit(275,6,17,18,-1,"chat-"); hit(292,6,17,18,-1,"chat+");
        // A (0040d330) / G (0040d220) toggles at window 111,2 and 141,2: UI1 30x41 cells, green 584/612 allowed, dark 647/677 blocked.
        ui_atlas(original_ui1_,x,y,111,2,30,41,trade_allowed_?584.f:647.f,267,30,41); hit(111,2,30,41,-1,"toggle-trade");
        ui_atlas(original_ui1_,x,y,141,2,30,41,invites_allowed_?612.f:677.f,267,30,41); hit(141,2,30,41,-1,"toggle-invites");
        // Their hover text names the click's effect: A 0x58e210 / 0x58e1a8, G 0x58e140 / 0x58e0c0 (white, 00404b40).
        if (hovered(ix+S(111),iy+S(2),S(30),S(41))) tip_={{trade_allowed_?"Oyuncularla Alışverişi Engelle":"Oyuncularla Alışverişe İzin Ver",white_rgba}};
        if (hovered(ix+S(141),iy+S(2),S(30),S(41))) tip_={{invites_allowed_?"Grup ve Lonca Davetlerini Engelle":"Grup ve Lonca Davetlerine İzin Ver",white_rgba}};
        const int left=ix+S(28),right=ix+S(290),top=iy+S(46);
        int line_y=iy+S(h-12)-lh();
        int skip=chat_scroll_; chat_link_hits_.clear();
        for (auto it=log_.rbegin();it!=log_.rend() && line_y>=top;++it) {
            if (it->channel<chat_system && chat_hidden_[static_cast<std::size_t>(it->channel)]) continue;
            const auto lines=wrap(it->text,right-left);
            const auto index=std::to_string(log_.size()-1-static_cast<std::size_t>(it-log_.rbegin()));
            for (auto line=lines.rbegin();line!=lines.rend() && line_y>=top;++line) {
                if (skip>0) { --skip; continue; }
                ui_->text(left,line_y,*line,it->color);
                if (!it->link.is_null()) chat_link_hits_.push_back({{left,line_y,right-left,lh()},log_.size()-1-static_cast<std::size_t>(it-log_.rbegin())});
                if (it->from) hits_.push_back({left,line_y,right-left,lh(),nullptr,-1,"chatline:"+index});
                line_y-=lh();
            }
        }
        if (skip>0) chat_scroll_-=skip; // clamp scrolling at the oldest retained line
        const int by=iy+S(h);
        const auto& channel=chat_channels[static_cast<std::size_t>(channel_)];
        const std::string prefix=std::string(channel.prefix)+(channel_==chat_whisper?"["+whisper_name_+"] ":"");
        std::string shown=input_;
        const int available=S(266)-ui_->text_width(prefix);
        while (!shown.empty() && ui_->text_width(shown+(editing_?"|":""))>available) {
            do shown.erase(0,1); while (!shown.empty() && (static_cast<unsigned char>(shown.front())&0xc0)==0x80);
        }
        ui_->text(ix+S(24),by+S(29.5f)-lh()/2,prefix+shown+(editing_?"|":""),editing_?original_rgba(channel.argb):dim_rgba);
        hits_.push_back({ix+S(19),by+S(18),S(272),S(24),nullptr,-1,"input"});
        hits_.push_back({ix+S(291),by+S(18),S(24),S(24),nullptr,-1,"send"});
        hits_.push_back({ix+S(18),by+S(44),S(16),S(15),nullptr,-1,"chat-up"});
        hits_.push_back({ix+S(34),by+S(44),S(16),S(15),nullptr,-1,"chat-down"});
        hits_.push_back({ix+S(50),by+S(44),S(16),S(15),nullptr,-1,"chat-end"});
        if (chat_menu_id_) { // 0040aad0 name menu above the chat window, titled with the clicked name
            std::vector<std::tuple<std::string,Json,std::string>> items; player_items(items,chat_menu_id_,true);
            const int row=lh()+S(10),mh=S(48)+static_cast<int>(items.size())*row,mx=ix+S(110),my=iy-mh;
            const auto content=frame(0,mx,my,S(190),mh,chat_menu_name_);
            hits_.push_back({mx+S(160),my,S(30),S(28),nullptr,-1,"chat-menu-close"});
            int ry=content[1];
            for (auto& [text,action,value]:items) { button(content[0],ry,text,std::move(action),-1,value,content[2]-content[0]); ry+=row; }
        }
        if (!chat_filter_open_) return;
        // 'F' filter list (helps.xml 'Mesaj Filtreleri'); Sistem is not filterable.
        const int row=lh()+S(6);
        const auto content=frame(0,ix+S(200),iy-S(48)-chat_system*row,S(170),S(48)+chat_system*row,"Filtre");
        for (std::size_t i=0;i<chat_hidden_.size();++i) {
            const int ry=content[1]+static_cast<int>(i)*row,box=lh()-S(4);
            outline(content[0],ry+S(2),box,box,dim_rgba);
            if (!chat_hidden_[i]) ui_->rect(content[0]+S(3),ry+S(5),box-S(6),box-S(6),original_rgba(chat_channels[i].argb));
            std::string name=chat_channels[i].prefix; name.resize(name.size()-2);
            ui_->text(content[0]+box+S(6),ry,name,original_rgba(chat_channels[i].argb));
            hits_.push_back({content[0],ry,content[2]-content[0],row,nullptr,-1,"filter:"+std::to_string(i)});
        }
    }
    const ikv::SourceSkill* source_skill(const Json& definition) const {
        if (!definition.contains("source")) return nullptr;
        const int ci=definition.at("source")[0],sn=definition.at("source")[1];
        const auto found=std::find_if(source_skills_.begin(),source_skills_.end(),[&](const auto& s){return s.classindex==ci && s.skillno==sn;});
        return found==source_skills_.end()?nullptr:&*found;
    }
    // Latest accepted state: events of a batch are handled before state_ is presented.
    const Json& skill_catalog() const {
        const auto& state=session_?session_->state:state_;
        return state.contains("catalogs") && state.at("catalogs").contains("skills")?state.at("catalogs").at("skills"):empty_object();
    }
    // Boz Ayı shares Kanatma's slot-8 rank and replaces it while the character flag named after it is set (exe 0044ccf0).
    std::string rank_key(const std::string& id) const { const auto f=skill_catalog().find(id); return f==skill_catalog().end()?id:f->value("replaces",id); }
    bool slotted(const std::string& id) const {
        const auto& st=character_state(state_); const auto f=skill_catalog().find(id);
        if (f==skill_catalog().end()) return false;
        if (f->contains("replaces")) return st.value(id,false);
        for (const auto& [other,o]:skill_catalog().items()) if (o.value("replaces",std::string{})==id && st.value(other,false)) return false;
        return true;
    }
    // Learned castable skills in original skill order: the bar before the character's first drop (NONCANONICAL convenience).
    std::vector<std::string> slot_skills() const {
        std::vector<std::pair<int,std::string>> learned;
        if (!session_) return {};
        const auto ranks=character_state(state_).value("skills",Json::object());
        for (const auto& [id,skill]:skill_catalog().items())
            if (skill.value("target",std::string{})!="passive" && slotted(id) && ranks.value(rank_key(id),0)>0) learned.emplace_back(skill.at("source")[1].get<int>(),id);
        std::sort(learned.begin(),learned.end());
        std::vector<std::string> result;
        for (auto& entry:learned) if (result.size()<10) result.push_back(std::move(entry.second));
        return result;
    }
    int skill_icon(const std::string& id) const {
        const auto found=skill_catalog().find(id);
        const auto* source=found==skill_catalog().end()?nullptr:source_skill(*found);
        return source?source->icon:-1;
    }
    int item_icon(const std::string& item) const {
        int icon=62; // Original pouch icon for native items without a source icon.
        const auto& items=state_.contains("catalogs")?state_.at("catalogs").value("items",Json::object()):Json::object();
        if (const auto definition=items.find(item);definition!=items.end() && definition->contains("source_uid")) {
            const auto source=definition->at("source_uid").get<unsigned>();
            const auto found=std::find_if(source_item_icons_.begin(),source_item_icons_.end(),[&](const auto& entry){return entry.first==source;});
            if (found!=source_item_icons_.end() && found->second>=0) icon=found->second;
        }
        return icon;
    }
    Json skill_action(const std::string& id) const {
        auto action=target_action("skill"); action["skill"]=id;
        const auto found=skill_catalog().find(id);
        const auto target=found==skill_catalog().end()?std::string{}:found->value("target",std::string{});
        const bool player_target=std::any_of(hud_entities_.begin(),hud_entities_.end(),[&](const Json& e){return e.value("id",0u)==target_ && e.contains("class_id");});
        if (target=="summon") { if (!target_) action.erase("target"); } // Boz Ayı recast: sent at the selected hostile
        else if (session_ && target!="enemy" && (target!="ally" || !player_target)) action["target"]=session_->self;
        return action;
    }
    // Hızlı Erişim Çubuğu: the server-held drag-and-drop layout (original 'tdbp' upload 0044b330, hcpe 0044b400 download).
    Json quickbar() const {
        const auto& st=character_state(state_);
        if (st.contains("quickbar")) return st.at("quickbar");
        Json slots=Json::array(); const auto skills=slot_skills();
        for (std::size_t i=0;i<10;++i) slots.push_back(i<skills.size()?Json{{"skill",skills[i]}}:Json());
        return slots;
    }
    // Keys 1..0 and a right click use a slot (helps.xml 'Hızlı Erişim Çubuğu'), at most every 300 ms (0044b520);
    // an item slot uses that bag item (itsu 00430610).
    void use_slot(std::size_t index) {
        const auto slot=quickbar().at(index);
        if (Clock::now()<slot_ready_) return;
        slot_ready_=Clock::now()+std::chrono::milliseconds(300);
        if (slot.contains("skill")) send(skill_action(slot.at("skill")));
        else if (slot.contains("item") && inventory_item(character_state(state_),slot.at("item"))) use_item(slot.at("item").get<std::string>());
    }
    const Json* item_def(const std::string& item) const {
        if (!state_.contains("catalogs") || !state_.at("catalogs").contains("items")) return nullptr;
        const auto& items=state_.at("catalogs").at("items"); const auto found=items.find(item);
        return found==items.end()?nullptr:&*found;
    }
    // The shown Banka tab (original page 0x611454): the server sends only that tab's cells ("bank" "tab").
    int bank_tab() const { const auto b=state_.find("bank"); return b!=state_.end() && b->is_object()?b->value("tab",0):0; }
    const Json* bank_item(const std::string& uid) const {
        if (state_.contains("bank") && state_.at("bank").is_object()) for (const auto& e:state_.at("bank").value("items",Json::array())) if (e.value("uid",std::string{})==uid) return &e;
        return nullptr;
    }
    // 0042dff0 as far as the native catalog carries it: wearable slot, classlimit, sexlimit and item level.
    bool wearable(const Json& entry) const {
        const Json* d=item_def(entry.value("item",std::string{})); const auto& c=state_.at("character");
        return d && d->contains("slot") && (!d->contains("class") || d->at("class").get<int>()+1==c.value("class_id",0)) &&
            (!d->contains("sex") || d->at("sex")==c.value("appearance",Json::object()).value("sex",1)) && character_state(state_).value("level",1)>=d->value("level",1);
    }
    // itsu 00430610: dead (0x58ece4), an item lying in the trade window (0x590ed0), else ui\eat.raw (UI sound slot 0x77d30c) and 'itsu'.
    void use_item(const std::string& uid) {
        if (dead()) notice("Ölüyken bunu yapamazsınız!");
        else if (std::any_of(trade_items_.begin(),trade_items_.end(),[&](const auto& i){return i.at("uid")==uid;})) notice("Bu eşyayı takas etmektesiniz!");
        else { audio_->play_event("ui\\eat.raw"); send(Json{{"action","use"},{"uid",uid}}); }
    }
    // 0042f690: Shift on a stackable cell holding more than one opens the amount box (window 0x77d33c, text 0x590d50) unless
    // an exchange is open (0042ed20 → 0x590db0); digits, then Enter (004336c0) or Evet.
    void open_split(const std::string& uid) {
        const Json* entry=inventory_item(character_state(state_),uid); const Json* d=entry?item_def(entry->value("item",std::string{})):nullptr;
        if (!d || d->value("stack",1)<=1 || entry->value("quantity",0)<=1) return;
        if (trade_.active()) { notice("Ticaret esnasında bunu yapamazsınız."); return; }
        confirm_=Json{{"action","split"},{"uid",uid}}; split_amount_.clear(); editing_=false;
        confirm_text_=std::to_string(entry->value("quantity",0))+" nesneden kaç tanesini ayırmak istersiniz?";
    }
    // 0042f690 Ctrl (DIK 0x1d/0x9d) → 0042e8c0: the item as a link on the current channel (not Rol yap), at most every 2 s
    // (else 0x590c60 "Bu yazışma kanalını gereksiz yere kirletmemeye özen gösteriniz.").
    void link_item(const std::string& uid) {
        if (std::string_view(chat_channels[static_cast<std::size_t>(channel_)].wire)=="roleplay") return; // Rol yap (original channel 5) is not in 0042e8c0's list
        if (Clock::now()<link_ready_) { chat_line("Bu yazışma kanalını gereksiz yere kirletmemeye özen gösteriniz.",original_rgba(chat_channels[chat_system].argb)); return; }
        Json action{{"action","chat"},{"channel",chat_channels[static_cast<std::size_t>(channel_)].wire},{"link",uid}};
        if (channel_==chat_whisper) { if (!whisper_to_) return; action["to"]=whisper_to_; }
        link_ready_=Clock::now()+std::chrono::seconds(2); send(action);
    }
    // 004344b0 vendor branch: target flag +0x82&0x40 (0x591f54), itemflag bit 2 (0x591f40), no price (0x591eb8), Klan Parası goods
    // without itemflag bit 4 (004311b0, 0x591e08), worn or banked cells (0x591c98), then the 004b8230 confirm 0x591d60 ('llps' on
    // Evet, 0042ddd0).
    void sell_item(const std::string& uid,bool banked) {
        const auto& st=character_state(state_);
        const Json* entry=banked?bank_item(uid):inventory_item(st,uid); if (!entry) return;
        const Json* d=item_def(entry->value("item",std::string{})); if (!d) return;
        const int flags=d->value("flags",0); const auto lira=d->value("price",std::int64_t{0}),clan=d->value("clan_price",std::int64_t{0});
        if (const Json* e=entity(target_); e && e->value("no_buy",false)) notice("Bu hedef, eşya satın almaz.");
        else if (flags&2) notice("Bu eşya satılamaz.");
        else if (lira<=0 && clan<=0) alert("Bu eşya satılamaz, çöpe atmayı veya başka oyuncuya satmayı deneyin.");
        else if (clan>0 && !((flags&3)==0 && (flags&4))) alert("Bu eşya klan parasıyla da satılamaz, çöpe atmayı veya başka oyuncuya satmayı deneyin.");
        else if (banked || !equipped_slot(st,uid).empty()) alert("Giyilmekte olan nesneler satılamaz; giysiyi önce üzerinizden çıkarın.");
        else {
            const int source=d->value("source_uid",0); // Teşkilat Bonosu 13615/13616 sell whole: 0x591d24
            ask(Json{{"action","sell"},{"target",target_},{"uid",uid},{"quantity",entry->value("quantity",1)}},
                "'"+entry_name(state_,*entry)+(source==13615 || source==13616?"' satılacak, emin misiniz?":"' 5'te biri(Plus paket sahiplerine 3'te biri) fiyatına satılacak, emin misiniz?"));
        }
    }
    // 004344b0: a bag, worn or bank cell released over a window cell (param_1) or over nothing (param_1 0 = destroy).
    void item_drop(float x,float y) {
        const auto& st=character_state(state_);
        const bool banked=drag_.contains("bank");
        const auto uid=drag_.value(banked?"bank":"item",std::string{}),worn=banked?std::string{}:equipped_slot(st,uid);
        const Json* entry=banked?bank_item(uid):inventory_item(st,uid); if (!entry) return;
        const Json* d=item_def(entry->value("item",std::string{}));
        const int flags=d?d->value("flags",0):0,quantity=entry->value("quantity",1);
        const auto window=std::find_if(window_rects_.rbegin(),window_rects_.rend(),[&](const auto& w){return inside(w.second,x,y);});
        if (window==window_rects_.rend()) {
            if (!worn.empty()) alert("Giyilmekte olan nesneler yok edilemez."); // 0x591890
            else if (flags&2) notice("Bu eşya yok edilemez."); // 0x5918e0
            else ask(Json{{"action",banked?"bank_destroy":"item_destroy"},{"uid",uid}},"Bu eşya kalıcı olarak yok edilecektir. Emin misiniz?"); // 0x591820
            return;
        }
        switch (window->first) {
        case win_bag:
            if (y<static_cast<float>(window->second[1]+S(182))) { // the worn silhouette above the 4x6 grid (cells start at y 185)
                if (banked) notice("Bankadaki bir eşyayı çantanıza almadan doğrudan giyemezsiniz!"); // 0x591a4c
                else if (!worn.empty()) alert("Giyilenler penceresi içerisinde taşıma yapamazsınız."); // 0x5918f8
                else if (dead()) notice("Ölüyken bunu yapamazsınız!");
                else if (!wearable(*entry)) alert("Bu eşyayı giyemezsiniz."); // 0x591964
                else send(Json{{"action","equip"},{"uid",uid}});
            } else if (banked) send(Json{{"action","bank_withdraw"},{"uid",uid},{"quantity",quantity}});
            else if (!worn.empty()) send(Json{{"action","unequip"},{"slot",worn}});
            else { // grid cell under the drop (9+33c, 185+33r): reorder (NONCANONICAL swap, see World item_move)
                const int col=static_cast<int>(std::floor((x-static_cast<float>(window->second[0]+S(9)))/static_cast<float>(S(33)))),row=static_cast<int>(std::floor((y-static_cast<float>(window->second[1]+S(185)))/static_cast<float>(S(33))));
                if (col>=0 && col<4 && row>=0 && row<6) send(Json{{"action","item_move"},{"uid",uid},{"to",4*scroll_[win_bag]+4*row+col}});
            }
            break;
        case win_bank:
            if (!worn.empty()) notice("Giyilen bir eşyayı çıkarmadan bankaya koyamazsınız!"); // 0x591a8c
            else if (!banked) send(Json{{"action","bank_deposit"},{"uid",uid},{"quantity",quantity},{"tab",bank_tab()}});
            break;
        case win_vendor: sell_item(uid,banked); break;
        case win_trade: { // 004344b0 → own cell 16+34c,27+34r (0043b8e0)
            if (banked) break;
            const int col=static_cast<int>(std::floor((x-static_cast<float>(window->second[0]+S(16)))/static_cast<float>(S(34)))),row=static_cast<int>(std::floor((y-static_cast<float>(window->second[1]+S(27)))/static_cast<float>(S(34))));
            if (col>=0 && col<3 && row>=0 && row<2) offer_item(uid,3*scroll_[win_trade]+3*row+col);
            break;
        }
        default: break;
        }
    }
    // Own Alışveriş offer = the authoritative own terms plus local edits; every edit is sent at once like the original
    // 'vmdt' drop and 'mmrt' money clicks (the server then clears both ✓, original state 5).
    void send_offer() { send(Json{{"action","trade_offer"},{"items",trade_items_},{"currency",trade_currency_}}); }
    bool own_trade_locked() const { // 0042e9e0 disables the own cells after ✓; nothing is offered before the session opens
        return !trade_.active() || trade_.offer().at("phase")!="open" || trade_.offer().at("own").at("accepted")==true;
    }
    // 004344b0 bag cell → trade cell checks in original order: one item per cell (0x591fdc/0x591f70), not worn (0x591bf0),
    // free target cell ("Hedef pencere doludur."), not bound (00431220, 0x591bbc). cell<0: the native bag shortcut.
    void offer_item(const std::string& uid,int cell=-1) {
        if (own_trade_locked()) return;
        const auto& st=character_state(state_);
        const Json* entry=inventory_item(st,uid); if (!entry) return;
        const Json* d=item_def(entry->value("item",std::string{}));
        const auto offered=std::any_of(trade_items_.begin(),trade_items_.end(),[&](const auto& i){return i.at("uid")==uid;});
        if (entry->value("quantity",1)>1) alert("Hücrede birden fazla eşya ile ticaret yapamazsınız!!!");
        else if (!equipped_slot(st,uid).empty()) alert("Giyilmekte olan nesnelerle alışveriş yapılamaz; giysiyi önce üzerinizden çıkarın.");
        else if (offered) return;
        else if (trade_items_.size()>=6 || (cell>=0 && cell<static_cast<int>(trade_items_.size()))) alert("Hedef pencere doludur.");
        else if (d && (d->value("flags",0)&6) && d->value("source_uid",0)!=14000) notice("Bu eşya size bağlanmıştır. Kimseye veremezsiniz.");
        else { trade_items_.push_back({{"uid",uid},{"quantity",1}}); send_offer(); }
    }
    void withdraw_item(const std::string& uid) {
        const auto found=std::find_if(trade_items_.begin(),trade_items_.end(),[&](const auto& i){return i.at("uid")==uid;});
        if (own_trade_locked() || found==trade_items_.end()) return;
        trade_items_.erase(found); send_offer();
    }
    // 0044bbc0: a skill or bag item dropped on a slot fills it; 0044bd70: a dragged slot swaps with the slot it is dropped
    // on, or empties when released outside the bar ("içeriğini tutup dışarı sürüklemeniz yeterlidir").
    void drop(float x,float y) {
        if (drag_.contains("trade")) { // an own trade cell dragged back onto the bag leaves the offer
            const auto bag=std::find_if(window_rects_.rbegin(),window_rects_.rend(),[&](const auto& w){return inside(w.second,x,y);});
            if (bag!=window_rects_.rend() && bag->first==win_bag) withdraw_item(drag_.at("item").get<std::string>());
            return;
        }
        auto slots=quickbar();
        const auto target=std::find_if(drags_.rbegin(),drags_.rend(),[&](const auto& d){return d.second.contains("slot") && inside(d.first,x,y);});
        if (drag_.contains("slot")) {
            const auto from=drag_.at("slot").get<std::size_t>();
            if (target==drags_.rend()) slots[from]=nullptr;
            else std::swap(slots[from],slots[target->second.at("slot").get<std::size_t>()]);
        } else if (target!=drags_.rend() && drag_.contains("skill")) slots[target->second.at("slot").get<std::size_t>()]=drag_;
        else if (target!=drags_.rend() && drag_.contains("item")) slots[target->second.at("slot").get<std::size_t>()]=Json{{"item",drag_.at("item")}};
        else { if (drag_.contains("item") || drag_.contains("bank")) item_drop(x,y); return; }
        send(Json{{"action","quickbar"},{"slots",std::move(slots)}});
    }
    void paint_slot_content(float x,float y,float size,const Json& slot) {
        if (slot.contains("skill")) {
            // Original 0044d010/0044cc50: a cooling slot's widget color becomes ARGB 0x99ff0000 and it shows remaining whole seconds (ms/1000+1).
            const auto id=slot.at("skill").get<std::string>(); const auto now=Clock::now();
            auto ready=skill_global_ready_;
            if (const auto found=skill_ready_.find(id);found!=skill_ready_.end()) ready=std::max(ready,found->second);
            source_icon(x,y,size,skill_icon(id),ready>now?0x99ff0000u:0xffffffffu);
            if (ready>now) centered(static_cast<int>(x+size/2),static_cast<int>(y+size/2)-S(4),std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(ready-now).count()/1000+1),{255,240,200,255});
        } else if (slot.contains("item") || slot.contains("bank")) {
            const Json* entry=slot.contains("bank")?bank_item(slot.at("bank").get<std::string>()):inventory_item(character_state(state_),slot.at("item"));
            if (!entry) return; // the original slot keeps the identity of a bag item that is gone
            source_icon(x,y,size,item_icon(entry->value("item",std::string{})));
            if (entry->value("quantity",0)>1) label(static_cast<int>(x+size)-S(10),static_cast<int>(y),std::to_string(entry->value("quantity",0)));
        }
    }
    std::array<int,4> bar_rect() const { const int w=S(352),h=S(64); return {(width_-w)/2,height_-S(4)-h,w,h}; }
    // Original action bar: move handles, UI1 tool tiles above ten numbered ability slots.
    void paint_bar() {
        const auto rect=bar_rect();
        const float x=static_cast<float>(rect[0]),y0=static_cast<float>(rect[1]),y1=y0+32*s_;
        hits_.push_back({rect[0]-S(30),rect[1],rect[2]+S(30),rect[3],nullptr,-1,{}});
        ui_atlas(original_ui1_,x,y0,0,0,32,32,224,592,32,32);
        ui_atlas(original_ui1_,x,y1,0,0,32,32,224,592,32,32);
        struct Tool { float u,v,w,h; int window; };
        // Original tooltip order (0x59b60c K, C, Y, U, H, G): head, bag, hand, anvil, compass, scroll; window 0 = H map toggle.
        static constexpr std::array<Tool,8> tools{{{16,640,32,32,win_character},{48,640,32,32,win_bag},{80,640,32,32,win_skills},
            {112,640,32,32,win_craft},{144,640,32,32,0},{176,640,32,32,win_quests},{321,626,27,22,win_guild},{324,566,25,21,win_settings}}};
        for (std::size_t i=0;i<tools.size();++i) {
            const auto& t=tools[i];
            const float tx=x+32*s_*static_cast<float>(i+1);
            ui_atlas(original_ui1_,tx,y0,(32-t.w)/2,(32-t.h)/2,t.w,t.h,t.u,t.v,t.w,t.h);
            if (t.window?is_open(t.window):map_open_) outline(static_cast<int>(tx),static_cast<int>(y0),S(32),S(32),{255,220,90,220});
            hits_.push_back({static_cast<int>(tx),static_cast<int>(y0),S(32),S(32),nullptr,t.window?t.window:-1,t.window?std::string{}:"map-toggle"});
        }
        const auto slots=quickbar();
        for (std::size_t i=0;i<10;++i) {
            const float sx=x+32*s_*static_cast<float>(i+1);
            const int ix=static_cast<int>(sx),iy=static_cast<int>(y1);
            ui_->rect(ix+S(1),iy+S(1),S(30),S(30),{140,140,130,210});
            ui_->rect(ix+S(2),iy+S(2),S(28),S(28),{0,0,0,235});
            if (!(dragging_ && drag_.contains("slot") && drag_.at("slot")==i)) paint_slot_content(sx+2*s_,y1+2*s_,28*s_,slots[i]);
            hits_.push_back({ix,iy,S(32),S(32),nullptr,-1,"slot:"+std::to_string(i)});
            drags_.push_back({{ix,iy,S(32),S(32)},Json{{"slot",i}}});
            label(ix+S(3),iy+S(31)-lh(),std::to_string((i+1)%10));
        }
        ui_atlas(original_ui1_,x,y1,-29,-11,27,22,292,626,27,22);
        hits_.push_back({static_cast<int>(x)-S(29),static_cast<int>(y1)-S(11),S(27),S(22),nullptr,-1,"help-book:Quickbar"}); // 0044c3a1 -> 0044b510
    }
    // helps.xml page range of the opened book (help_book_), or every page.
    std::pair<int,int> help_range() const {
        const auto& pages=scene_.help;
        if (help_book_.empty()) return {0,static_cast<int>(pages.size())};
        const auto first=std::find_if(pages.begin(),pages.end(),[&](const auto& p){return p.book==help_book_;});
        const auto last=std::find_if(first,pages.end(),[&](const auto& p){return p.book!=help_book_;});
        return {static_cast<int>(first-pages.begin()),static_cast<int>(last-pages.begin())};
    }
    // 00426c10: open the Yardım window on one helps.xml book.
    void open_help(const std::string& book) {
        if (std::none_of(scene_.help.begin(),scene_.help.end(),[&](const auto& p){return p.book==book;})) return;
        help_book_=book; help_page_=help_range().first; scroll_[win_help]=0; toggle_window(win_help,true);
    }
    // Tutorial books while the own character is below level 2 (00477fd0 vs 0058d7bc = 2.0): entering the world opens Seviye1a
    // (004b63b4), selecting Agah Efendi (0x58e938) opens Seviye1b (select 0041b090 -> 0041b24f); talking to him Seviye1c (dialogue).
    void tutorial() {
        if (!session_ || !has_snapshot_ || character_state(state_).value("level",2)>=2) return;
        if (!tutorial_entered_) { tutorial_entered_=true; open_help("Seviye1a"); }
        if (target_==tutorial_target_) return;
        tutorial_target_=target_;
        if (const Json* e=entity(target_); e && e->value("name",std::string{})=="Agah Efendi") open_help("Seviye1b");
    }
    int window_width(int id,int available) const {
        if (id==win_bag) return S(150);
        if (id==win_character) return S(250);
        if (id==win_quests) return S(223);
        if (id==win_craft) return S(224);
        if (id==win_bank) return S(290);
        if (id==win_skills) return S(130); // original root 130x274 (0044e070)
        if (id==win_help) return std::min(available,S(900));
        return S(320);
    }
    void paint_windows(const Json& st) {
        const auto bar=bar_rect();
        const int top=S(8),bottom=bar[1]-S(6),left=S(560),available=width_-S(4)-left; // right of the target frame (300+168) and its effect grid (to 556)
        // The original Alışveriş window is not stacked: 0043b8e0 centres it at ((screen width-304)/2, 160).
        auto total=[&]{ int sum=0; for (const int id:windows_) if (id!=win_trade) sum+=window_width(id,available)+S(4); return sum; };
        while (windows_.size()>1u+is_open(win_trade) && total()>available)
            windows_.erase(std::find_if(windows_.begin(),windows_.end(),[](int id){return id!=win_trade;}));
        std::vector<int> order;
        for (const int id:{static_cast<int>(win_bag),static_cast<int>(win_character)}) if (is_open(id)) order.push_back(id);
        for (auto it=windows_.rbegin();it!=windows_.rend();++it) if (*it!=win_bag && *it!=win_character && *it!=win_trade) order.push_back(*it);
        // Yetenekler: 15 rows + detail text; Üretim/Banka: original UI2 art heights (Banka + native money/withdraw strip).
        static constexpr std::array<int,win_contacts+1> desired{0,650,420,380,0,300,575,340,0,580,760,346,410,360}; // Ayarlar: 7 sliders + Vulkan upscaler + frame generation
        int x=width_-S(4),bag_x=-1;
        for (const int id:order) {
            const int w=window_width(id,available);
            x-=w;
            const int h=id==win_help?bottom-top:std::min(bottom-top,S(static_cast<float>(desired[static_cast<std::size_t>(id)])));
            if (id==win_bag) { paint_bag(x,top,h,st); bag_x=x; }
            else if (id==win_character) paint_character(x,top,st);
            else if (id==win_quests) paint_quests(x,top,st);
            else if (id==win_craft) paint_craft(x,top,st);
            else if (id==win_bank) paint_bank(x,top,h,st);
            else if (id==win_skills) paint_skills(x,top,st);
            else paint_panel(id,frame(id,x,top,w,h,window_title(id)),st);
            x-=S(4);
        }
        if (is_open(win_trade) && trade_.active()) paint_trade((width_-S(304))/2,S(160));
        // The selected item's tooltip is topmost: windows painted after the bag (Satıcı, Yardım) must not cover it.
        if (const Json* selected=bag_x>=0?inventory_item(st,selected_uid_):nullptr) paint_item_tooltip(bag_x,top,*selected,st,state_);
    }
    static std::string window_title(int id) {
        switch (id) {
        case win_vendor: return "Satıcı";
        case win_group: return "Grup";
        case win_settings: return "Ayarlar";
        case win_guild: return "Klan ve Lonca";
        case win_contacts: return "Bağlantılar";
        case win_help: return "Yardım";
        default: return "Yetenekler";
        }
    }
    const Json* inventory_item(const Json& st,const std::string& uid) const {
        if (st.contains("inventory")) for (const auto& item:st.at("inventory")) if (item.value("uid",std::string{})==uid) return &item;
        return nullptr;
    }
    std::string equipped_slot(const Json& st,const std::string& uid) const {
        if (st.contains("equipment") && st.at("equipment").is_object())
            for (const auto& [slot,owned]:st.at("equipment").items()) if (owned==uid) return slot;
        return {};
    }
    // Original Karakter Bilgisi (00402f60): UI2 770,3 frame, exe format strings 0x58d462-0x58dcb0, rows at VOD 10:00/18:30 positions.
    void paint_character(int x,int y,const Json& st) {
        const float fx=static_cast<float>(x),fy=static_cast<float>(y);
        const Json stats=state_.value("stats",Json::object());
        const int resist_points=stats.value("resistance_points",0),craft_points=stats.value("craft_points",0);
        int resist_left=resist_points,craft_left=craft_points;
        for (const auto& [stat,count]:stat_pending_) (stat=="ates"||stat=="buz"||stat=="elektrik"||stat=="asit"||stat=="zehir"?resist_left:craft_left)-=count;
        ui_atlas(original_ui2_,fx,fy,0,0,250,580,770,3,250,580);
        if (resist_left>0 || craft_left>0) ui_atlas(original_ui2_,fx,fy,0,469,96,107,770,583,96,107); // VOD 18:30: lamp lit while points are unspent
        if (!stat_pending_.empty()) ui_atlas(original_ui2_,fx,fy,92,469,156,107,862,583,156,107); // widget +0x500 (00404e1d): lit ✓ while pending (00402d5d)
        hits_.push_back({x,y,S(250),S(580),nullptr,-1,{}});
        hits_.push_back({x+S(220),y,S(30),S(28),nullptr,win_character,{}});
        hits_.push_back({x+S(211),y+S(7),S(16),S(16),nullptr,-1,"help-book:Stats"}); // '?' 004051e4 -> 00402a20
        window_rects_.push_back({win_character,{x,y,S(250),S(580)}});
        const int px=ui_->pixel_size(); ui_->set_pixel_size(small_px());
        auto at=[&](float ax,float ay,const std::string& text,Rgba color=white_rgba){ label(x+S(ax),y+S(ay)-lh()/2,text,color); };
        auto heading=[&](float ax,float ay,const std::string& text){ ui_->set_bold(true); at(ax,ay,text); ui_->set_bold(false); }; // VOD 18:30 bold rows
        const auto& character=state_.contains("character")?state_.at("character"):empty_object();
        auto f=[&](const char* key){ return st.value(key,0.0); };
        static constexpr std::array<const char*,3> classes{"Savaşçı","Büyücü","Şifacı"}; // exe 0x5993c8-0x5993e8, class_id order of the launcher
        const auto class_name=classes[static_cast<std::size_t>(std::clamp(character.value("class_id",1),1,3)-1)];
        constexpr float right=127.5f;
        at(14,19,"Karakter Bilgisi");
        heading(25,48.5f,character.value("name",std::string{}));
        // NONCANONICAL ranks (server stats: Şan clan rank, Lonca Reisi/Üye) right of the name; no original rank strings exist.
        std::string rank=stats.value("clan_rank",std::string{});
        if (stats.contains("guild_rank")) rank+=(rank.empty()?"":" · ")+stats.value("guild_rank",std::string{});
        if (!rank.empty()) label(x+S(242)-ui_->text_width(rank),y+S(48.5f)-lh()/2,rank,original_rgba(0xff33cccc)); // right-aligned: clear of a 16-letter name
        const bool in_guild=st.contains("guild") && st.at("guild").is_object();
        // 004043b0: "Lonca  :" alone, else "Lonca  : <%s><%d>" with the lonca name and level.
        at(9,66.5f,in_guild?printf_text("Lonca  : <%s><%d>",st.at("guild").value("name",std::string{}).c_str(),st.at("guild").value("level",1)):"Lonca  :");
        // Puan: cumulative XP over the next cumulative threshold (VOD 18:30 "Puan: 1811/3183"); state xp counts within the level.
        const double xp_total=state_.value("xp_total",0.0);
        at(9,82.5f,printf_text("Seviye %.0f %s",f("level"),class_name)); at(right,82.5f,printf_text("Puan: %.0f/%.0f",xp_total+f("xp"),xp_total+state_.value("xp_interval",0.0)));
        at(9,97.5f,printf_text("Klan  : %s",clan_name(st.value("clan",0)))); at(right,97.5f,printf_text("Şan   : %.0f",f("san"))); // 00402f60 rows
        // Karizma (attribute 24) and LP (67) are server-sent values with no client rule: native Karizma is the config constant, LP 0.
        at(9,113,printf_text("Karizma   : %.0f",stats.value("charisma",0.0))); at(right,113,printf_text("LP    : %.0f",0.0));
        heading(9,140,"Özellikler :");
        at(9,158,printf_text("Enerji : %.0f/%.0f",f("hp"),f("max_hp"))); at(right,158,printf_text("Kudret : %.0f/%.0f",f("mana"),f("max_mana")));
        at(9,173,printf_text("Saldırı : %.1f",stats.value("attack",0.0)/10)); at(right,173,printf_text("Savunma : %.1f",stats.value("defense",0.0)/10)); // tenths
        // 00402f60 getter ids (original 004338f0 names): worn Efsun sums arrive in hundredths (stats.efsun); crit as tenths/multiplier.
        const Json efsun_sum=stats.value("efsun",Json::object());
        const auto efsun_of=[&](std::initializer_list<int> ids){double v=0;for(int id:ids)v+=efsun_sum.value(std::to_string(id),0)/100.0;return v;};
        const Json crit=stats.value("crit",Json::array({0,2}));
        const Json armor=stats.value("armor",Json::array({0,60}));
        at(9,188,printf_text("Saldırı Hızı : %.2f",std::max(.5,stats.value("attack_cooldown",20.0)/20.0))); at(right,188,printf_text("Zırh : %%%.1f/%%%.0f",armor[0].get<double>()/10,armor[1].get<double>()));
        // 00402f60: Büyü Kritik = Büyü Kritik Şansı (37) + 1 / x(Büyü Kritik Çarpanı (38) + 2); Konsantrasyon rank terms not ported.
        at(9,203,printf_text("Kritik : %%%.1f/x%.0f",crit[0].get<double>()/10,crit[1].get<double>())); at(right,203,printf_text("Büyü Kritik : %%%.1f/x%.0f",1.0+efsun_of({37}),2.0+efsun_of({38})));
        const Json damage=stats.value("weapon_damage",Json::array({0,0}));
        at(9,218,printf_text("Silah Hasarı : %.0f-%.0f",damage[0].get<double>(),damage[1].get<double>()));
        heading(9,245.5f,"Silah Efsunları :"); heading(right,245.5f,"Büyü Efsunları :");
        // 00402f60 colours and getters: Max 39, Ateş 48, Buz 49, Elektrik 50, Asit 52, Zehir 51; Büyü = 31 + 58/53/54/55/57/56, Şifa 62.
        static constexpr std::array<std::tuple<const char*,std::uint32_t,int,int>,7> efsun{{{"Max",0xffffffff,39,58},{"Ateş",0xffe4cd05,48,53},{"Buz",0xff00e4ff,49,54},
            {"Elektrik",0xfff6a7fd,50,55},{"Asit",0xff08c781,52,57},{"Zehir",0xff57ab0c,51,56},{"Şifa",0xffff5d5d,0,62}}};
        for (std::size_t i=0;i<efsun.size();++i) {
            const float ry=262.5f+15*static_cast<float>(i);
            const auto& [name,argb,weapon_id,magic_id]=efsun[i];
            if (i<6) at(9,ry,printf_text("%s : %.0f",name,efsun_of({weapon_id})),original_rgba(argb));
            at(right,ry,printf_text("Büyü %s : %.0f",i?name:"Fiziksel",i<6?efsun_of({31,magic_id}):efsun_of({magic_id})),original_rgba(argb));
        }
        // Dirençler / Üretim / Toplama rows sit beside the frame's red -/+ buttons (centres x 18.5/36.5, right group +119.5).
        const Json resist=st.value("resistances",Json::object()),craft=st.value("crafts",Json::object());
        struct Row { const char* stat; const char* name; float column,ry; bool resistance; };
        static constexpr std::array<Row,11> rows{{{"ates","Ateş",0,386,true},{"buz","Buz",0,409,true},{"elektrik","Elektrik",0,431,true},
            {"asit","Asit",119.5f,386,true},{"zehir","Zehir",119.5f,409,true},{"silahtar","Silahtar",0,478.5f,false},{"zirhci","Zırhçı",0,500,false},
            {"kimyaci","Kimyacı",0,522.5f,false},{"madenci","Madenci",119.5f,478.5f,false},{"sarraf","Sarraf",119.5f,500,false},{"lokman","Lokman",119.5f,522.5f,false}}};
        heading(9,368.5f,"Dirençler :"); heading(9,460,"Üretim :"); heading(right,460,"Toplama :");
        for (const auto& r:rows) {
            static constexpr std::array<std::pair<const char*,int>,5> resist_ids{{{"ates",12},{"buz",13},{"elektrik",14},{"zehir",15},{"asit",16}}};
            int value=(r.resistance?resist:craft).value(r.stat,0);
            for (const auto& [stat,attribute]:resist_ids) if (r.resistance && r.stat==std::string_view(stat)) value+=static_cast<int>(std::lround(efsun_of({attribute})));
            at(47.5f+r.column,r.ry,r.name);
            const auto pending=stat_pending_.find(r.stat); const int added=pending==stat_pending_.end()?0:pending->second;
            label(std::max(x+S(89+r.column),x+S(47.5f+r.column)+ui_->text_width(r.name)+S(3)),y+S(r.ry)-lh()/2,printf_text(": %d",value+added),white_rgba);
            if (added>0) hits_.push_back({x+S(10.5f+r.column),y+S(r.ry-8),S(16),S(16),nullptr,-1,std::string("stat-minus:")+r.stat});
            if ((r.resistance?resist_left:craft_left)>0) hits_.push_back({x+S(28.5f+r.column),y+S(r.ry-8),S(16),S(16),nullptr,-1,std::string("stat-plus:")+r.stat});
        }
        hits_.push_back({x+S(209),y+S(537),S(39),S(39),nullptr,-1,"stat-ok"}); // ✓: widget 92,469 156x107 hit from its +0x180/+0x184 = 117,68 (00491773)
        at(9,446,"Puan"); at(42.5f,446,printf_text(": %d",resist_left));
        at(9,536.5f,"Puan"); at(42.5f,536.5f,printf_text(": %d",craft_left));
        ui_->set_pixel_size(px);
    }
    const Json& quest_catalog() const {
        return state_.contains("catalogs") && state_.at("catalogs").contains("quests")?state_.at("catalogs").at("quests"):empty_object();
    }
    using Lines=std::vector<std::pair<std::string,Rgba>>;
    // Explicit newlines, then word wrap; an empty paragraph keeps a blank line.
    void add_lines(Lines& lines,const std::string& text,Rgba color,int width) {
        for (std::size_t start=0;;) {
            const auto end=text.find('\n',start);
            auto wrapped=wrap(text.substr(start,end==std::string::npos?std::string::npos:end-start),width);
            if (wrapped.empty()) lines.emplace_back(std::string{},color);
            for (auto& line:wrapped) lines.emplace_back(std::move(line),color);
            if (end==std::string::npos) return;
            start=end+1;
        }
    }
    int draw_lines(int x,int y,int bottom,const Lines& lines,std::size_t skip) {
        for (auto i=skip;i<lines.size() && y+lh()<=bottom;++i,y+=lh()) ui_->text(x,y,lines[i].first,lines[i].second);
        return y;
    }
    // Original quest text (00448dd0): title, Yapman gerekenler, objectives, Açıklama, description, blank lines between.
    Lines quest_lines(const std::string& id,const Json& st,int width) {
        Lines lines;
        if (!quest_catalog().contains(id)) return lines;
        const auto& quest=quest_catalog().at(id);
        const Json objective=quest.value("objective",Json::object()),reward=quest.value("reward",Json::object());
        const Json progress=st.contains("quests") && st.at("quests").contains(id)?st.at("quests").at(id):Json();
        std::string goal=quest.value("objective_text",std::string{});
        if (quest_done(st,id)) goal+=" (tamamlandı)";
        else if (progress.is_object()) {
            const auto kind=objective.value("kind",std::string{}); // talk/visit are one step; kill/collect/use count (n/m)
            if (kind!="talk" && kind!="visit") goal+=" ("+std::to_string(progress.value("progress",0))+"/"+std::to_string(objective.value("count",0))+")";
            if (progress.contains("deadline")) { // NONCANONICAL countdown for timed quests (VOD 44:35 "görevinin süresi doldu!")
                const auto left=(std::max<std::int64_t>(0,progress.value("deadline",0ll)-state_.value("tick",0ll))+19)/20;
                goal+="\nKalan süre: "+std::to_string(left/60)+":"+(left%60<10?"0":"")+std::to_string(left%60);
            }
        }
        // The original window shows no reward line (server-only rule); the native reward is listed so it is visible before accepting.
        // A reward lists every class's line (W-REW): another class's weapon (class limit, no sex variants) is not shown; armour is
        // handed out as this character's class/sex variant of the same name.
        std::string prize="Ödül: "+std::to_string(reward.value("xp",0))+" tecrübe, "+std::to_string(reward.value("currency",0))+" para";
        const int own_class=state_.at("character").value("class_id",0);
        for (const auto& item:reward.value("items",Json::array())) {
            const Json* d=item_def(item.value("item",std::string{}));
            if (!d || (d->contains("class") && !d->contains("sex") && d->at("class").get<int>()+1!=own_class)) continue; // the server sends own-class lines only
            if (item.contains("classes") && std::ranges::find(item.at("classes"),Json(own_class))==item.at("classes").end()) continue; // W-REW line of another class page
            prize+=", "+std::to_string(item.value("quantity",1))+" "+item_name(state_,item.value("item",std::string{}));
        }
        const std::array<std::pair<std::string,Rgba>,4> sections{{{quest.value("name",id)+" ["+std::to_string(quest.value("level",1))+"]",quest_title_rgba},
            {"Yapman gerekenler\n\n"+goal,white_rgba},{"Açıklama\n\n"+quest.value("description",std::string{}),white_rgba},{prize,dim_rgba}}};
        for (const auto& [text,color]:sections) {
            const auto first=lines.size();
            add_lines(lines,text,color,width); lines.emplace_back(std::string{},color);
            if (color==white_rgba) lines[first].second=quest_header_rgba; // section header line
        }
        return lines;
    }
    static bool quest_done(const Json& st,const std::string& id) {
        const auto done=st.value("quests_done",Json::array());
        return std::find(done.begin(),done.end(),Json(id))!=done.end();
    }
    // Görev al pages at this NPC: quests it gives that can be taken now (auto quests only at the auto-quest NPC, GB-Anı)
    // and accepted quests handed in here (turn_in, default giver). Instanced copies of an NPC sit at id + n*65536.
    std::vector<std::string> dialogue_quests(const Json& offered,std::uint32_t target,const Json& st) const {
        const auto is=[&](const Json& npc){ if (!npc.is_number_unsigned()) return false; const auto c=npc.get<std::uint32_t>(); return target==c || (target>c && (target-c)%65536==0); };
        std::vector<std::string> pages;
        for (const auto& quest:offered) {
            if (!quest.is_string() || !quest_catalog().contains(quest.get<std::string>())) continue;
            const auto& id=quest.get_ref<const std::string&>(); const auto& q=quest_catalog().at(id);
            const bool active=st.contains("quests") && st.at("quests").contains(id);
            bool open=!q.contains("requires") || !q.at("requires").is_string() || quest_done(st,q.at("requires").get<std::string>());
            for (const auto& also:q.value("requires_also",Json::array())) open=open && also.is_string() && quest_done(st,also.get<std::string>());
            const bool accept=!active && !quest_done(st,id) && open && q.value("auto",false)!=is(q.value("giver",Json()));
            if (accept || (active && is(q.contains("turn_in")?q.at("turn_in"):q.value("giver",Json())))) pages.push_back(id);
        }
        return pages;
    }
    // Original Görevler (G; UI2 5,612 frame, VOD 08:16): accepted quests above, the selected quest's text below, trash = Seçili görevi sil.
    void paint_quests(int x,int y,const Json& st) {
        ui_atlas(original_ui2_,static_cast<float>(x),static_cast<float>(y),0,0,223,409,5,612,223,409);
        hits_.push_back({x,y,S(223),S(409),nullptr,-1,{}});
        hits_.push_back({x+S(198),y+S(7),S(22),S(22),nullptr,win_quests,{}});
        hits_.push_back({x+S(189),y+S(7),S(16),S(16),nullptr,-1,"help-book:Quests"}); // '?' 0044a422 -> 004486a0
        window_rects_.push_back({win_quests,{x,y,S(223),S(409)}});
        const int px=ui_->pixel_size(); ui_->set_pixel_size(small_px());
        label(x+S(18),y+S(18)-lh()/2,"Görevler",white_rgba);
        std::vector<std::string> active;
        if (st.contains("quests") && st.at("quests").is_object())
            for (const auto& [id,quest]:st.at("quests").items()) if (quest_catalog().contains(id)) active.push_back(id);
        if (std::find(active.begin(),active.end(),selected_quest_)==active.end()) selected_quest_=active.empty()?std::string{}:active.front();
        auto& scroll=scroll_[win_quests];
        scroll=std::clamp(scroll,0,std::max(0,static_cast<int>(active.size())-1));
        int ly=y+S(43);
        for (std::size_t i=static_cast<std::size_t>(scroll);i<active.size() && ly+lh()<=y+S(128);++i) {
            const auto& quest=quest_catalog().at(active[i]);
            if (active[i]==selected_quest_) ui_->rect(x+S(15),ly,S(193),lh(),{255,255,255,36});
            label(x+S(18),ly,fit(quest.value("name",active[i])+" ["+std::to_string(quest.value("level",1))+"]",S(187)),quest_title_rgba);
            hits_.push_back({x+S(15),ly,S(193),lh(),nullptr,-1,"quest:"+active[i]});
            ly+=lh();
        }
        hits_.push_back({x+S(17),y+S(140),S(16),S(16),nullptr,-1,"quest-up"});
        hits_.push_back({x+S(33),y+S(140),S(16),S(16),nullptr,-1,"quest-down"});
        hits_.push_back({x+S(17),y+S(380),S(16),S(16),nullptr,-1,"quest-text-up"});
        hits_.push_back({x+S(33),y+S(380),S(16),S(16),nullptr,-1,"quest-text-down"});
        if (selected_quest_.empty()) paragraph(x+S(18),y+S(174),S(187),y+S(370),"Kabul edilmiş görevin yok.",dim_rgba);
        else {
            hits_.push_back({x+S(179),y+S(138),S(20),S(20),nullptr,-1,"quest-delete"});
            const auto lines=quest_lines(selected_quest_,st,S(187));
            quest_text_scroll_=std::clamp(quest_text_scroll_,0,std::max(0,static_cast<int>(lines.size())-1));
            draw_lines(x+S(18),y+S(174),y+S(370),lines,static_cast<std::size_t>(quest_text_scroll_));
        }
        ui_->set_pixel_size(px);
    }
    // NPC Konuş window (VOD 52:30 'Görev al'): greeting page, then one page per offered quest; ←/→ page, ✓ accept or complete, ✗ Görevi reddet.
    void paint_dialogue(const Json& st) {
        if (dialogue_.is_null()) return;
        const auto target=dialogue_.value("target",0u);
        const Json* npc=entity(target);
        if (!npc) { dialogue_=nullptr; return; } // the NPC left view
        const auto quests=dialogue_quests(dialogue_.value("quests",Json::array()),target,st);
        const int pages=1+static_cast<int>(quests.size());
        if (dialogue_page_<0) dialogue_page_=quests.empty()?0:1; // the event's frame can precede the first applied state_
        dialogue_page_=std::clamp(dialogue_page_,0,pages-1);
        const int w=S(213),x=(width_-w)/2,y=S(140),tab=S(14),width=w-S(14);
        const int px=ui_->pixel_size(); ui_->set_pixel_size(small_px());
        Lines lines;
        if (dialogue_page_) lines=quest_lines(quests[static_cast<std::size_t>(dialogue_page_-1)],st,width);
        else add_lines(lines,dialogue_.value("text",std::string{}),white_rgba,width);
        // VOD size 213×221 at 1080p; longer native texts grow the window instead of clipping.
        const float hb=std::max(221.f,static_cast<float>(S(20)+static_cast<int>(lines.size())*lh())/s_);
        const int h=S(hb);
        const std::string title=dialogue_page_?"Görev al":npc->value("name",std::string{});
        const int tw=ui_->text_width(title)+S(10);
        constexpr Rgba fill{14,20,26,215},edge{117,116,121,255};
        ui_->rect(x,y-tab,tw,tab,fill); outline(x,y-tab,tw,tab,edge);
        ui_->rect(x,y,w,h,fill); outline(x,y,w,h,edge);
        ui_->text(x+S(5),y-tab+(tab-lh())/2,title,white_rgba);
        hits_.push_back({x,y-tab,w,h+tab+S(10),nullptr,-1,{}});
        draw_lines(x+S(7),y+S(8),y+h-S(8),lines,0);
        // UI1 ←/→ and red ✓/✗ tiles straddle the bottom edge as in the VOD.
        const float fx=static_cast<float>(x),fy=static_cast<float>(y);
        if (pages>1) {
            ui_atlas(original_ui1_,fx,fy,5,hb-6,12,13,99,754,12,13); ui_atlas(original_ui1_,fx,fy,19,hb-6,12,13,113,754,12,13);
            hits_.push_back({x+S(5),y+S(hb-6),S(12),S(13),nullptr,-1,"dialogue-"});
            hits_.push_back({x+S(19),y+S(hb-6),S(12),S(13),nullptr,-1,"dialogue+"});
            label(x+S(36),y+h-lh()/2,std::to_string(dialogue_page_+1)+"/"+std::to_string(pages),white_rgba); // VOD 16:30 page count
        }
        if (dialogue_page_) {
            const auto& id=quests[static_cast<std::size_t>(dialogue_page_-1)];
            const Json progress=st.contains("quests") && st.at("quests").contains(id)?st.at("quests").at(id):Json();
            const Json objective=quest_catalog().at(id).value("objective",Json::object());
            const int count=objective.value("kind",std::string{})=="talk"?1:objective.value("count",0);
            Json action;
            if (!progress.is_object()) action=Json{{"action","quest_accept"},{"target",target},{"quest",id}}; // Görevi kabul et
            else if (progress.value("progress",0)>=count) action=Json{{"action","quest_complete"},{"target",target},{"quest",id}};
            if (!action.is_null()) {
                ui_atlas(original_ui1_,fx,fy,180,hb-7,15,15,64,688,15,15);
                hits_.push_back({x+S(180),y+S(hb-7),S(15),S(15),std::move(action),-1,{}});
            }
        }
        ui_atlas(original_ui1_,fx,fy,196,hb-7,15,15,81,688,15,15);
        hits_.push_back({x+S(196),y+S(hb-7),S(15),S(15),nullptr,-1,"dialogue-close"}); // Görevi reddet
        ui_->set_pixel_size(px);
    }
    // Ganimet window 00436140: UI2 235,580 120x134 at 200,100, caption "Ganimet" at 11,5, close button 16x16 at 102,4 (0042de30),
    // nine 32x32 slots at 9+33*(i%3), 26+33*(i/3). A slot click asks the server for the item; the window closes with the box.
    void paint_loot() {
        if (!loot_.is_object()) return;
        if (const Json* box=entity(loot_.value("target",0u)); !box || box->value("looted",true)) { loot_=nullptr; return; }
        ui_atlas(original_ui2_,0,0,200,100,120,134,235,580,120,134);
        hits_.push_back({S(200),S(100),S(120),S(134),nullptr,-1,{}});
        label(S(211),S(105),"Ganimet",gold_rgba);
        hits_.push_back({S(302),S(104),S(16),S(16),nullptr,-1,"loot-close"});
        const auto& items=loot_.at("items");
        for (std::size_t i=0;i<std::min<std::size_t>(9,items.size());++i) {
            if (!items[i].is_object()) continue;
            const float sx=209+33.f*static_cast<float>(i%3),sy=126+33.f*static_cast<float>(i/3);
            source_icon(sx*s_,sy*s_,32*s_,catalog_icon(items[i].value("item",std::string{})));
            if (const int count=items[i].value("quantity",1); count>1) label(S(sx+2),S(sy+31)-lh(),std::to_string(count));
            hits_.push_back({S(sx),S(sy),S(32),S(32),nullptr,-1,"loot-slot:"+std::to_string(i)});
        }
    }
    // Item tooltip 0x437590 for the selected bag item, beside the bag: title "<prefix1> <prefix2> <name>" coloured by prefix
    // count (one 0xFF00DEFF, two 0xFFD2FF00), Hasar/Saldırı Hızı/Kritik, Zırh, "Eskime: [%.0f/%.0f]", Fiyat, each prefix
    // attribute "[%s]: %.3f" (0xffc0c040, names 004338f0), then Gereksinimler with Seviye green/red and Sınıf/Cinsiyet.
    // `source`: the state whose catalogs name the item (state_, or a chat link's own catalog entry).
    void paint_item_tooltip(int bag_x,int bag_y,const Json& entry,const Json& st,const Json& source) {
        const auto& items=source.at("catalogs").at("items");
        const auto found=items.find(entry.value("item",std::string{}));
        if (found==items.end()) return;
        const Json& d=*found;
        static const std::map<int,const char*> names{{4,"Enerji"},{5,"Kudret"},{6,"Maksimum Enerji"},{7,"Maksimum Kudret"},{8,"Güç"},{9,"Beceri"},{10,"Zeka"},{11,"Bünye"},
            {12,"Direnç(Ateş)"},{13,"Direnç(Buz)"},{14,"Direnç(Elektrik)"},{15,"Direnç(Zehir)"},{16,"Direnç(Asit)"},{17,"Zırh"},{25,"Enerji Rejenarasyonu"},{26,"Kudret Rejenarasyonu"},
            {27,"Saldırı"},{28,"Saldırı(Yakın)"},{29,"Saldırı(Menzilli)"},{30,"Savunma"},{31,"Büyü Hasarı (Hepsi)"},{32,"Kritik Zırhı"},{33,"Saldırı Hızı"},{37,"Büyü Kritik Şansı"},
            {38,"Büyü Kritik Çarpanı"},{39,"Maksimum Hasar"},{40,"Maksimum Hasar(Yakın)"},{41,"Maksimum Hasar(Menzilli)"},{42,"Kritik Vuruş İhtimali"},{45,"Kritik Hasar Çarpanı"},
            {48,"Hasar(Ateş)"},{49,"Hasar(Buz)"},{50,"Hasar(Elektrik)"},{51,"Hasar(Zehir)"},{52,"Hasar(Asit)"},{53,"Büyü Hasarı(Ateş)"},{54,"Büyü Hasarı(Buz)"},
            {55,"Büyü Hasarı(Elektrik)"},{56,"Büyü Hasarı(Zehir)"},{57,"Büyü Hasarı(Asit)"},{58,"Büyü Hasarı(Fiziksel)"},{59,"Saldırı Menzili"},{61,"Hareket Hızı"},{62,"İyileştirme Büyüleri"}};
        std::vector<std::pair<std::string,std::uint32_t>> lines;
        const auto prefixes=entry_prefixes(source,entry);
        lines.push_back({entry_name(source,entry),prefixes.empty()?0xffffffffu:prefixes.size()==1?0xff00deffu:0xffd2ff00u});
        lines.push_back({" ",0xfff5f5f5});
        if (d.contains("detail")) { // glyph type (0x592908) -> "[Tılsım]" (0x592318), then the classes.dat detail ("%%" printf-escaped)
            lines.push_back({"[Tılsım]",0xfff5f5f5});
            std::string detail=d.at("detail").get<std::string>();
            for (auto at=detail.find("%%");at!=std::string::npos;at=detail.find("%%",at+1)) detail.erase(at,1);
            for (auto& line:wrap(detail,S(220))) lines.push_back({std::move(line),0xfff5f5f5});
        }
        if (d.contains("damage_range")) {
            lines.push_back({printf_text("Hasar: %.0f-%.0f",d.at("damage_range")[0].get<double>(),d.at("damage_range")[1].get<double>()),0xfff5f5f5});
            lines.push_back({printf_text("Saldırı Hızı: %.2f",d.value("attack_cooldown",20)/20.0),0xfff5f5f5});
            if (d.contains("crit")) lines.push_back({printf_text("Kritik Vuruş Yüzdesi: %.1fx%.2f",d.at("crit")[0].get<double>(),d.at("crit")[1].get<double>()),0xfff5f5f5});
        }
        if (d.contains("armor")) lines.push_back({printf_text("Zırh: %.3f",d.at("armor").get<double>()/10000),0xfff5f5f5}); // generator armor = dmgreduction in basis points
        if (d.contains("durability")) lines.push_back({printf_text("Eskime: [%.0f/%.0f]",entry.value("durability",d.at("durability").get<double>()),d.at("durability").get<double>()),0xffc0c0c0});
        // 0x438326..0x438474: "Fiyat: %s Akçe" when akceprice > 0, else "Fiyat: %s Lira" or "Fiyat: %s Lira / %s Klan Parası" (pvpprice).
        if (d.contains("akce_price")) lines.push_back({"Fiyat: "+std::to_string(d.at("akce_price").get<int>())+" Akçe",0xfff5f5f5});
        else lines.push_back({"Fiyat: "+std::to_string(d.value("price",0))+" Lira"+(d.contains("clan_price")?" / "+std::to_string(d.at("clan_price").get<int>())+" Klan Parası":std::string{}),0xfff5f5f5});
        if (!prefixes.empty()) lines.push_back({" ",0xfff5f5f5});
        for (const auto& p:prefixes) if (const auto f=source_prefixes.find(p.get<int>()); f!=source_prefixes.end())
            for (const auto& [attribute,value]:f->second.attributes) { const auto n=names.find(attribute); lines.push_back({printf_text("[%s]: %.3f",n!=names.end()?n->second:std::to_string(attribute).c_str(),value),0xffc0c040}); }
        lines.push_back({" ",0xfff5f5f5}); lines.push_back({"Gereksinimler:",0xfff5f5f5});
        lines.push_back({printf_text("Seviye: [%.0f]",d.value("level",1.0)),st.value("level",1)<d.value("level",1)?0xffe30000u:0xff00dc00u});
        static constexpr std::array<const char*,3> classes{"[Savaşçı]","[Büyücü]","[Şifacı]"}; // 0x5920d0..
        if (d.contains("class")) lines.push_back({std::string("Sınıf: ")+classes[static_cast<std::size_t>(std::clamp(d.at("class").get<int>(),0,2))],0xfff5f5f5});
        if (d.contains("sex")) lines.push_back({std::string("Cinsiyet: ")+(d.at("sex").get<int>()?"[Erkek]":"[Kadın]"),0xfff5f5f5});
        int width=0; for (const auto& line:lines) width=std::max(width,ui_->text_width(line.first));
        const int w=width+S(16),hgt=lh()*static_cast<int>(lines.size())+S(12),left=std::max(0,bag_x-w-S(4));
        bag_y=std::max(0,std::min(bag_y,height_-hgt)); // on screen (chat links sit at the bottom)
        ui_->rect(left,bag_y,w,hgt,{8,8,12,230}); outline(left,bag_y,w,hgt,{150,130,90,255});
        int ly=bag_y+S(6); for (const auto& [text,c]:lines) { ui_->text(left+S(8),ly,text,original_rgba(c)); ly+=lh(); }
    }
    // Original UI2 Eşyalar bag: equipment silhouette and 4x6 grid; native items beyond 24 scroll.
    void paint_bag(int x,int y,int h,const Json& st) {
        const float fx=static_cast<float>(x),fy=static_cast<float>(y);
        ui_atlas(original_ui2_,fx,fy,0,0,150,395,242,177,150,395);
        hits_.push_back({x,y,S(150),S(395),nullptr,-1,{}});
        hits_.push_back({x+S(124),y,S(26),S(24),nullptr,win_bag,{}});
        hits_.push_back({x+S(113),y+S(7),S(16),S(16),nullptr,-1,"help-book:Inventory"}); // '?' 0043ddbe -> 0042de40
        window_rects_.push_back({win_bag,{x,y,S(150),h}});
        label(x+S(10),y+S(13)-lh()/2,"Eşyalar",gold_rgba);
        if (session_ && st.contains("inventory") && state_.contains("catalogs") && state_.at("catalogs").contains("items")) {
            const auto& items=state_.at("catalogs").at("items");
            static constexpr std::array<std::array<float,4>,13> equipment{{
                {0,0,0,0},{0,0,0,0},{42,59,32,32},{42,93,32,32},{8,61,32,32},{42,126,32,32},
                {0,0,0,0},{42,29,32,32},{11,95,16,16},{25,94,16,16},{25,43,16,16},{76,44,32,32},{75,77,32,32}
            }};
            int cell=0;
            const int first=4*scroll_[win_bag];
            for (const auto& item:st.at("inventory")) {
                const auto uid=item.value("uid",std::string{});
                const auto definition=items.find(item.value("item",std::string{}));
                const int icon=item_icon(item.value("item",std::string{}));
                float sx=0,sy=0,size=32;
                const int source_slot=definition!=items.end()?definition->value("source_slot",-1):-1;
                if (!equipped_slot(st,uid).empty() && source_slot>=0 && source_slot<static_cast<int>(equipment.size()) &&
                    equipment[static_cast<std::size_t>(source_slot)][2]>0) {
                    const auto& position=equipment[static_cast<std::size_t>(source_slot)];
                    sx=position[0]; sy=position[1]; size=position[2];
                } else {
                    const int index=cell++-first;
                    if (index<0 || index>=24) continue;
                    sx=9+33*static_cast<float>(index&3); sy=185+33*static_cast<float>(index>>2);
                }
                source_icon(fx+sx*s_,fy+sy*s_,size*s_,icon);
                const int ix=x+S(sx),iy=y+S(sy);
                if (item.value("quantity",0)>1) label(ix+S(2),iy+S(size)-lh(),std::to_string(item.value("quantity",0)));
                if (uid==selected_uid_) outline(ix,iy,S(size),S(size),{255,220,90,255});
                hits_.push_back({ix,iy,S(size),S(size),nullptr,-1,"uid:"+uid});
                drags_.push_back({{ix,iy,S(size),S(size)},Json{{"item",uid}}}); // bag and worn cells drag (quickbar, windows, 004344b0)
            }
            if (cell<=first && scroll_[win_bag]>0) scroll_[win_bag]=std::max(0,(cell-1)/4);
        }
        // Selected item actions below the original bag.
        const Json* selected=inventory_item(st,selected_uid_);
        const auto content=frame(0,x,y+S(395),S(150),h-S(395),selected?entry_name(state_,*selected)+
            " x"+std::to_string(selected->value("quantity",0)):"Eşya seçin");
        const int w=content[2]-content[0],row=lh()+S(8);
        int ry=content[1];
        auto action_row=[&](const std::string& text,Json action,std::string value={}) {
            if (ry+row>content[3]-3*lh()) return;
            button(content[0],ry,text,std::move(action),-1,std::move(value),w); ry+=row;
        };
        if (selected) {
            const auto slot=equipped_slot(st,selected_uid_);
            if (!slot.empty()) action_row("Çıkar",Json{{"action","unequip"},{"slot",slot}});
            else {
                // Native shortcuts for the original gestures (drag onto the silhouette/vendor, Shift-click split): same checks.
                action_row("Kuşan",nullptr,"item-equip");
                action_row("Kullan",nullptr,"item-use");
                action_row("Sat",nullptr,"item-sell");
                if (selected->value("quantity",1)>1) action_row("Böl",nullptr,"item-split");
                if (is_open(win_bank)) action_row("Bankaya koy",Json{{"action","bank_deposit"},{"uid",selected_uid_},{"quantity",selected->value("quantity",1)},{"tab",bank_tab()}});
                const bool offered=std::any_of(trade_items_.begin(),trade_items_.end(),[&](const auto& i){return i.at("uid")==selected_uid_;});
                action_row(offered?"Takastan çıkar":"Takasa ekle",nullptr,"offer-item");
            }
        }
        // Eşyalar money rows (0x43b7c3..0x43b82d): Lira, "Klan Parası:" (attribute 64), "Akçe:" (66, İKV Dükkan; no native source),
        // each with its 0x43e1ba/0x43e413/0x43e66c hover text.
        static constexpr std::array<std::pair<const char*,const char*>,3> money{{{"currency","Para: "},{"clan_currency","Klan Parası: "},{"akce","Akçe: "}}};
        static constexpr std::array<const char*,3> money_tips{"En fazla 999 milyon paranız olabilir.","En fazla 999 milyon klan paranız olabilir.",
            "Oyundayken İKV Dükkan'da yapacağınız Akçe işlemleri buraya yansımaz. Çıkıp girince güncellenir."};
        float mouse_x=0,mouse_y=0; SDL_GetMouseState(&mouse_x,&mouse_y);
        const int mx=static_cast<int>(mouse_x),my=static_cast<int>(mouse_y);
        const int px=ui_->pixel_size(); ui_->set_pixel_size(small_px()); // window text size: "Klan Parası: 999000000" fits the 150-wide bag
        for (std::size_t i=0;i<money.size();++i) {
            const int money_y=content[3]-static_cast<int>(3-i)*lh();
            ui_->text(content[0],money_y,money[i].second+std::to_string(st.value(money[i].first,0)),gold_rgba);
            if (mx>=content[0] && mx<content[2] && my>=money_y && my<money_y+lh()) {
                const int tip_w=ui_->text_width(money_tips[i])+S(16),tip_y=money_y-lh()-S(12),tip_x=std::clamp(content[0]-tip_w/3,0,std::max(0,width_-tip_w));
                ui_->rect(tip_x,tip_y,tip_w,lh()+S(8),{8,8,12,230}); outline(tip_x,tip_y,tip_w,lh()+S(8),{150,130,90,255});
                ui_->text(tip_x+S(8),tip_y+S(4),money_tips[i],white_rgba);
            }
        }
        ui_->set_pixel_size(px);
    }
    // Catalog item -> original icon selector (classes.dat icon by source_uid; pouch 62 when unknown).
    int catalog_icon(const std::string& item) const {
        if (!state_.contains("catalogs") || !state_.at("catalogs").contains("items")) return 62;
        const auto& items=state_.at("catalogs").at("items");
        const auto definition=items.find(item);
        if (definition==items.end() || !definition->contains("source_uid")) return 62;
        const auto source=definition->at("source_uid").get<unsigned>();
        const auto found=std::find_if(source_item_icons_.begin(),source_item_icons_.end(),[&](const auto& entry){return entry.first==source;});
        return found!=source_item_icons_.end() && found->second>=0?found->second:62;
    }
    // Original Üretim window (UI2 542,376 224x346, help Üretim strings 0x592864..0x5928d8): ←/→ profession page
    // (crafttype table 0x5d6150), learned recipe list, ↑/↓ scroll, trash "Seçili reçeteyi hafızamdan sil", anvil "Seçili reçetedekini üret".
    void paint_craft(int x,int y,const Json& st) {
        static constexpr std::array<const char*,6> stats{"silahtar","zirhci","kimyaci","madenci","sarraf","lokman"};
        static constexpr std::array<const char*,6> names{"Silahtar","Zırhçı","Kimyacı","Madenci","Sarraf","Lokman"};
        const float fx=static_cast<float>(x),fy=static_cast<float>(y);
        ui_atlas(original_ui2_,fx,fy,0,0,224,346,542,376,224,346);
        hits_.push_back({x,y,S(224),S(346),nullptr,-1,{}});
        hits_.push_back({x+S(203),y+S(7),S(15),S(15),nullptr,win_craft,{}});
        hits_.push_back({x+S(189),y+S(7),S(16),S(16),nullptr,-1,"help-book:Craft"}); // '?' 00440681 -> 0042e0f0
        window_rects_.push_back({win_craft,{x,y,S(224),S(346)}});
        label(x+S(14),y+S(14)-lh()/2,"Üretim",gold_rgba);
        hits_.push_back({x+S(18),y+S(30),S(16),S(16),nullptr,-1,"craft-page-"});
        hits_.push_back({x+S(36),y+S(30),S(16),S(16),nullptr,-1,"craft-page+"});
        const auto page=static_cast<std::size_t>(craft_page_);
        const int level=st.value("crafts",Json::object()).value(stats[page],0);
        label(x+S(58),y+S(38)-lh()/2,std::string(names[page])+": "+std::to_string(level),white_rgba);
        const int px=ui_->pixel_size(); ui_->set_pixel_size(small_px());
        const int left=x+S(19),right=x+S(205),row=std::max(S(22),lh()+S(4));
        int ry=y+S(66);
        const Json recipes=state_.contains("catalogs")?state_.at("catalogs").value("recipes",Json::object()):Json::object();
        std::vector<std::string> shown;
        for (const auto& id:st.value("recipes",Json::array())) if (id.is_string() && recipes.contains(id.get<std::string>()) && recipes.at(id.get<std::string>()).value("craft",std::string{})==stats[page]) shown.push_back(id.get<std::string>());
        auto& scroll=scroll_[win_craft]; scroll=std::clamp(scroll,0,std::max(0,static_cast<int>(shown.size())-1));
        hits_.push_back({x+S(18),y+S(312),S(16),S(16),nullptr,-1,"craft-scroll-"});
        hits_.push_back({x+S(36),y+S(312),S(16),S(16),nullptr,-1,"craft-scroll+"});
        if (shown.empty()) paragraph(left,ry,right-left,y+S(180),"Bu meslekte bildiğiniz reçete yok. Reçeteyi Eşyalar'da seçip Kullan'a basın.",dim_rgba);
        for (std::size_t i=static_cast<std::size_t>(scroll);i<shown.size() && ry+row<=y+S(180);++i,ry+=row) {
            const auto& recipe=recipes.at(shown[i]); const auto product=recipe.value("product",std::string{});
            source_icon(static_cast<float>(left),static_cast<float>(ry),20*s_,catalog_icon(product));
            ui_->text(left+S(24),ry+(S(20)-lh())/2,fit(item_name(state_,product)+" (Sv "+std::to_string(recipe.value("level",0))+")",right-left-S(26)),
                shown[i]==craft_selected_?gold_rgba:recipe.value("level",0)<=level?text_rgba:dim_rgba);
            hits_.push_back({left,ry,right-left,row,nullptr,-1,"recipe:"+shown[i]});
        }
        // Selected recipe: its inputs with owned/needed counts.
        if (recipes.contains(craft_selected_) && std::find(shown.begin(),shown.end(),craft_selected_)!=shown.end()) {
            int dy=y+S(186);
            ui_->text(left,dy,"Gerekenler:",gold_rgba); dy+=lh()+S(2);
            for (const auto& c:recipes.at(craft_selected_).value("components",Json::array())) {
                if (dy+S(18)>y+S(292)) break;
                const auto item=c.value("item",std::string{}); int have=0;
                for (const auto& entry:st.value("inventory",Json::array())) if (entry.value("item",std::string{})==item) have+=entry.value("quantity",0);
                const int need=c.value("quantity",0);
                source_icon(static_cast<float>(left),static_cast<float>(dy),16*s_,catalog_icon(item));
                ui_->text(left+S(20),dy+(S(16)-lh())/2,fit(item_name(state_,item)+" "+std::to_string(have)+"/"+std::to_string(need),right-left-S(22)),have>=need?text_rgba:Rgba{230,90,80,255});
                dy+=S(18);
            }
        }
        ui_->set_pixel_size(px);
        hits_.push_back({x+S(146),y+S(312),S(23),S(24),nullptr,-1,"craft-forget"}); // trash: Seçili reçeteyi hafızamdan sil
        hits_.push_back({x+S(185),y+S(312),S(23),S(24),nullptr,-1,"craft-make"});   // anvil: Seçili reçetedekini üret
    }
    // Original Banka window (UI2 233,720 290x298, 8x8 cells; Help_Bank.dds "Banka"). Two tabs ("Kasanız iki sekmeden
    // oluşmaktadır", guide l.880): 00441950 places the ← (95,4) / → (112,4) 16-px arrows, 00440110/004401d0 step the page
    // 0x611454 within 0..1 and retitle "Banka %d"; 0043f330 shows bank cells page·64..page·64+63. The money strip below is
    // NONCANONICAL (the original window has none).
    void paint_bank(int x,int y,int h,const Json& st) {
        const float fx=static_cast<float>(x),fy=static_cast<float>(y);
        ui_atlas(original_ui2_,fx,fy,0,0,290,298,233,720,290,298);
        hits_.push_back({x,y,S(290),S(298),nullptr,-1,{}});
        hits_.push_back({x+S(268),y+S(2),S(20),S(20),nullptr,win_bank,{}});
        hits_.push_back({x+S(95),y+S(4),S(16),S(16),nullptr,-1,"bank-tab:-"});
        hits_.push_back({x+S(112),y+S(4),S(16),S(16),nullptr,-1,"bank-tab:+"});
        window_rects_.push_back({win_bank,{x,y,S(290),h}});
        label(x+S(19),y+S(14)-lh()/2,"Banka "+std::to_string(bank_tab()+1),gold_rgba);
        const Json bank=state_.value("bank",Json());
        if (!bank.is_object()) {
            paragraph(x+S(24),y+S(40),S(240),y+S(290),"Banka kapalı. Bir Bankacıya sağ tıklayıp İşlemler → Kasamı göster seçin.",white_rgba);
        } else {
            int index=0;
            for (const auto& entry:bank.value("items",Json::array())) {
                if (index>=64) break;
                const float sx=16+33.3f*static_cast<float>(index%8),sy=27+33.1f*static_cast<float>(index/8); ++index;
                const auto uid=entry.value("uid",std::string{});
                source_icon(fx+sx*s_,fy+sy*s_,28*s_,catalog_icon(entry.value("item",std::string{})));
                const int ix=x+S(sx),iy=y+S(sy);
                if (entry.value("quantity",0)>1) label(ix+S(2),iy+S(28)-lh(),std::to_string(entry.value("quantity",0)));
                if (uid==bank_selected_) outline(ix,iy,S(28),S(28),{255,220,90,255});
                hits_.push_back({ix,iy,S(28),S(28),nullptr,-1,"bank-item:"+uid});
                drags_.push_back({{ix,iy,S(28),S(28)},Json{{"bank",uid}}});
            }
        }
        const std::int64_t stored=bank.is_object()?bank.value("currency",std::int64_t{0}):0,carried=st.value("currency",std::int64_t{0});
        const auto content=frame(0,x,y+S(298),S(290),h-S(298),"Kasa: "+std::to_string(stored)+"  Para: "+std::to_string(carried));
        if (!bank.is_object()) return;
        Flow f{content[0],content[2],content[0],content[1]};
        const auto money=[&](const char* text,std::int64_t amount){ if (amount) flow(f,text,Json{{"action","bank_currency"},{"amount",amount}}); };
        if (!bank_selected_.empty()) flow(f,"Çantaya al",nullptr,-1,"bank-take");
        money("Yatır 100",std::min<std::int64_t>(100,carried)); money("Tümünü yatır",carried);
        money("Çek 100",-std::min<std::int64_t>(100,stored)); money("Tümünü çek",-stored);
    }
    // The class's fifteen skills.dat slots in grid order (record i = skillno i); Boz Ayı takes slot 8 while its character flag is
    // set (004a6720 → 0044b9b0). Ranks are the invested points of the slot's rank key.
    struct SkillSlot { std::string id; const ikv::SourceSkill* record=nullptr; int rank=0; };
    std::array<SkillSlot,15> skill_slots(const Json& st) const {
        std::array<SkillSlot,15> slots{};
        const auto& catalog=skill_catalog(); const auto ranks=st.value("skills",Json::object());
        for (const auto& [key,definition]:catalog.items()) if (const int n=definition.at("source")[1]; n<15 && !definition.contains("replaces")) slots[static_cast<std::size_t>(n)].id=key;
        for (const auto& [key,definition]:catalog.items()) if (definition.contains("replaces") && slotted(key))
            for (auto& slot:slots) if (slot.id==definition.at("replaces").get<std::string>()) slot.id=key;
        for (auto& slot:slots) if (!slot.id.empty()) { slot.record=source_skill(catalog.at(slot.id)); slot.rank=ranks.value(rank_key(slot.id),0); }
        return slots;
    }
    // Original Yetenekler (0044e070; analysis/porting-wave5/Skills5/skillwin-layout.md): UI2 frame 399,46 130x274 with the 3x5 grid
    // of 32 px skills.dat icons at 15+32c,53+33r (row r needs level 10r, else 0xff666666; the selected one 0xff77ff77, 0044cf50),
    // the rank under each (pending points 0xff77ff77, 0044d120), "Kalan:" = skill points − pending (0044d070) with the lamp
    // 153,203 lit while > 0, the top glow 287,53 and the lit ✓ 505,9 (else 473,9) while points are pending (0044d1f0), and
    // invisible ?/×/−/+ hotspots over the frame art. ✓ (0044d4e0) sends every pending point at once ('clxs' → skill_learn).
    void paint_skills(int x,int y,const Json& st) {
        const float fx=static_cast<float>(x),fy=static_cast<float>(y);
        const auto slots=skill_slots(st);
        const int level=st.value("level",0),pending=std::accumulate(skill_pending_.begin(),skill_pending_.end(),0);
        const int left=state_.value("skill_points",0)-pending;
        ui_atlas(original_ui2_,fx,fy,0,0,130,274,399,46,130,274);
        if (pending) ui_atlas(original_ui2_,fx,fy,23,1,106,110,287,53,106,110);
        if (left>0) ui_atlas(original_ui2_,fx,fy,0,175,86,96,153,203,86,96);
        ui_atlas(original_ui2_,fx,fy,88,241,30,31,pending?505.f:473.f,9,30,31);
        hits_.push_back({x,y,S(130),S(274),nullptr,-1,{}});
        label(x+S(11),y+S(5),"Yetenekler");
        const int px=ui_->pixel_size(); ui_->set_pixel_size(small_px()); // rank and Kalan labels are verdana 10 (0x44ebf8, 40×16 boxes)
        int hovered_slot=-1;
        for (std::size_t i=0;i<slots.size();++i) {
            const auto& slot=slots[i]; if (!slot.record) continue;
            const int row=static_cast<int>(i/3),ix=x+S(15.f+32.f*static_cast<float>(i%3)),iy=y+S(53.f+33.f*static_cast<float>(row));
            source_icon(static_cast<float>(ix),static_cast<float>(iy),32*s_,slot.record->icon,static_cast<int>(i)==skill_slot_?0xff77ff77u:level>=10*row?0xffffffffu:0xff666666u);
            label(ix,iy+S(16),std::to_string(slot.rank+skill_pending_[i]),skill_pending_[i]?original_rgba(0xff77ff77):text_rgba);
            hits_.push_back({ix,iy,S(32),S(32),nullptr,-1,"skill-pick:"+std::to_string(i)});
            if (hovered(ix,iy,S(32),S(32))) hovered_slot=static_cast<int>(i);
            if (slot.rank>0 && skill_catalog().at(slot.id).value("target",std::string{})!="passive") drags_.push_back({{ix,iy,S(32),S(32)},Json{{"skill",slot.id}}});
        }
        label(x+S(32),y+S(226),"Kalan:"); label(x+S(64),y+S(226),std::to_string(left));
        ui_->set_pixel_size(px);
        hits_.push_back({x+S(82),y+S(4),S(16),S(16),nullptr,-1,"help-book:Skills"}); // 0044cdb0
        hits_.push_back({x+S(100),y+S(4),S(16),S(16),nullptr,-1,"skill-close"}); // 0044d6e0
        hits_.push_back({x+S(48),y+S(243),S(16),S(16),nullptr,-1,"skill-minus"}); // 0044d480
        hits_.push_back({x+S(67),y+S(243),S(16),S(16),nullptr,-1,"skill-plus"}); // 0044d470 → 0044d2a0
        hits_.push_back({x+S(88),y+S(241),S(30),S(31),nullptr,-1,"skill-ok"}); // 0044d720 → 0044d4e0
        // NONCANONICAL: the Boz Ayı flag's official grant rule is unrecovered, so a Savaşçı swaps slot 8 on demand (below the frame).
        for (const auto& [key,definition]:skill_catalog().items())
            if (definition.contains("replaces") && level>=definition.value("level",0)) button(x,y+S(278),"Kanatma <> Boz Ayı",Json{{"action","skill_variant"},{"skill",key}});
        // 0044e070 gives every icon window its own tooltip (0044de50: skills.dat name 0xff00ff00, then the detail split on '|' in
        // white, unwrapped; "%%" is a literal percent), shown while the pointer is over that icon.
        if (hovered_slot<0) return;
        const auto& record=*slots[static_cast<std::size_t>(hovered_slot)].record;
        tip_={{record.name,original_rgba(0xff00ff00)}};
        std::string line;
        for (std::size_t i=0;i<=record.detail.size();++i) {
            if (i==record.detail.size() || record.detail[i]=='|') { tip_.push_back({std::move(line),white_rgba}); line.clear(); }
            else if (record.detail.compare(i,2,"%%")==0) { line+='%'; ++i; }
            else line+=record.detail[i];
        }
    }
    void paint_panel(int id,const std::array<int,4>& content,const Json& st) {
        const auto [left,top,right,bottom]=content;
        const int width=right-left;
        int y=top;
        auto& scroll=scroll_[static_cast<std::size_t>(id)];
        if (id==win_vendor) {
            const Json* subject=entity(target_);
            if (!subject || !subject->contains("shop") || subject->at("shop").empty()) {
                paragraph(left,y,width,bottom,"Bir satıcıyı hedefleyin (sağ tık: İşlemler → Satıcı). Satmak için Eşyalar'da eşya seçip Sat'a basın.",dim_rgba);
                return;
            }
            ui_->text(left,y,fit(subject->value("name",std::string{}),width),gold_rgba); y+=lh()+S(4);
            int index=0;
            for (const auto& entry:subject->at("shop")) {
                if (!entry.is_string() || index++<scroll) continue;
                if (y+lh()+S(8)>bottom) break;
                const auto item=entry.get<std::string>();
                std::string text="Satın al: "+item_name(state_,item);
                if (state_.contains("catalogs") && state_.at("catalogs").contains("items") && state_.at("catalogs").at("items").contains(item)) {
                    const auto& d=state_.at("catalogs").at("items").at(item);
                    text+=" — "+(d.contains("akce_price")?std::to_string(d.at("akce_price").get<int>())+" Akçe":std::to_string(d.value("price",0))+" Lira"+
                        (d.contains("clan_price")?" / "+std::to_string(d.at("clan_price").get<int>())+" Klan Parası":std::string{}));
                }
                button(left,y,text,nullptr,-1,"buy:"+item,width); // client checks 00431700 first
                y+=lh()+S(10);
            }
            paragraph(left,y+S(4),width,bottom,"Satmak için Eşyalar'da eşya seçip Sat'a basın.",dim_rgba);
        } else if (id==win_group) {
            Flow f{left,right,left,y};
            flow(f,"Hedefi davet et",target_action("group_invite"));
            flow(f,"Daveti kabul et",Json{{"action","group_accept"}});
            flow(f,"Gruptan Çık",nullptr,-1,"group-leave");
            y=flow_end(f)+S(6);
            ui_->text(left,y,group_members_.empty()?"Grupta değilsiniz.":"Üyeler:",gold_rgba); y+=lh();
            for (const auto& member:group_members_) {
                if (!member.is_number_unsigned()) continue;
                const auto member_id=member.get<std::uint32_t>();
                const Json* e=entity(member_id);
                std::string text=session_ && member_id==session_->self?state_.at("character").value("name",std::string{}):
                    e?e->value("name",std::string{}):"#"+std::to_string(member_id);
                if (y+lh()<=bottom) ui_->text(left,y,fit(text,width));
                y+=lh();
            }
        } else if (id==win_guild) {
            // Klan tier (char-info rows 0x58daec/0x58dad0, "Klan Parası:" 0x5925e8) above the player-made Lonca.
            const int clan=st.value("clan",0);
            const Json stats=state_.value("stats",Json::object()); // NONCANONICAL clan_rank/guild_rank (world.cpp state stats)
            ui_->text(left,y,printf_text("Klan  : %s",clan_name(clan)),clan?original_rgba(0xff33cccc):text_rgba); y+=lh();
            if (clan) {
                ui_->text(left,y,printf_text("Şan   : %.0f",st.value("san",0.0))); y+=lh();
                if (stats.contains("clan_rank")) { ui_->text(left,y,"Rütbe : "+stats.value("clan_rank",std::string{})); y+=lh(); }
                ui_->text(left,y,printf_text("Klan Parası: %.0f",st.value("clan_currency",0.0))); y+=lh()+S(6);
            } else y=paragraph(left,y,width,bottom,"Klana katılmak için Arzın Çocukları veya Lodos muhafızına sağ tıklayıp Klana Katıl'ı seçin.",dim_rgba)+S(6);
            if (st.contains("guild") && st.at("guild").is_object()) {
                const auto& guild=st.at("guild");
                ui_->text(left,y,fit("Lonca  : <"+guild.value("name",std::string{})+"><"+std::to_string(guild.value("level",1))+">",width),gold_rgba); y+=lh();
                ui_->text(left,y,fit(std::to_string(guild.value("members",Json::array()).size())+" üye · "+stats.value("guild_rank",std::string{}),width)); y+=lh();
                ui_->text(left,y,printf_text("Lonca Parası: %.0f",guild.value("funds",0.0))); y+=lh()+S(6); // guild_donate pool (NONCANONICAL)
            } else y=paragraph(left,y,width,bottom,"Loncanız yok. Lonca adını sohbet satırına yazıp Lonca kur'a basın.",dim_rgba)+S(6);
            Flow f{left,right,left,y};
            flow(f,"Lonca kur",nullptr,-1,"guild-create");
            flow(f,"Hedefi davet et",target_action("guild_invite"));
            flow(f,"Daveti kabul et",Json{{"action","guild_accept"}});
            flow(f,"Ayrıl",Json{{"action","guild_leave"}});
            flow(f,"Hedefi çıkar",target_action("guild_kick"));
            flow(f,"Bağış yap",nullptr,-1,"guild-donate");
        } else if (id==win_contacts) {
            // (B)Bağlantılar (0x58e28c, help "Bağlantı Listesi"): Arkadaşlar/Yasaklılar lists, "dışarıda" for offline friends.
            Flow f{left,right,left,y};
            flow(f,contacts_tab_?"Arkadaşlar":"[Arkadaşlar]",nullptr,-1,"contacts:0");
            flow(f,contacts_tab_?"[Yasaklılar]":"Yasaklılar",nullptr,-1,"contacts:1");
            y=flow_end(f)+S(4);
            const auto list=contacts_tab_?"ignored":"friends";
            const auto entries=st.value("contacts",Json::object()).value(list,Json::array());
            const auto online=state_.value("contacts_online",Json::array());
            if (entries.empty()) paragraph(left,y,width,bottom,"Listeye eklemek için oyuncuya sağ tıklayıp Arkadaşlara Ekle veya Yasaklılara Ekle'yi seçin.",dim_rgba);
            int index=0;
            for (const auto& entry:entries) {
                if (index++<scroll || y+lh()+S(8)>bottom) continue;
                const bool here=std::find(online.begin(),online.end(),entry[0])!=online.end();
                const auto remove=Json{{"action","contact_remove"},{"list",list},{"character_id",entry[0]}};
                const int bw=button(right-ui_->text_width("Listeden sil")-S(16),y,"Listeden sil",remove);
                ui_->text(left,y+S(3),fit(entry[1].get<std::string>()+(contacts_tab_ || here?"":"  dışarıda"),width-bw-S(6)),contacts_tab_?dim_rgba:here?original_rgba(0xff00ff00):dim_rgba);
                y+=lh()+S(10);
            }
        } else if (id==win_settings) {
            // Original Ayarlar (VOD 00:05, strings 0x58f134..0x58f208, apply 0041c07d): seven 0..1 sliders, ✓ keeps, ✗ restores.
            static constexpr std::array<const char*,7> names{"Arkaplan müziği","Ses Efektleri","Karakter Detay Mes.","Görüş Mesafesi",
                "Ortam Efektleri","Karakter Efektleri","Ölü Yok Olma Süresi"};
            for (int i=0;i<7;++i) {
                ui_->text(left,y,names[static_cast<std::size_t>(i)]); y+=lh()+S(2);
                const int cell=std::max(1,width/11),value=static_cast<int>(std::lround(setting(i)*10));
                for (int k=0;k<=10;++k) {
                    ui_->rect(left+k*cell,y,cell-S(1),S(10),k<=value?Rgba{200,170,90,235}:Rgba{40,44,48,235});
                    hits_.push_back({left+k*cell,y-S(3),cell,S(16),nullptr,-1,"pref:"+std::to_string(i)+":"+std::to_string(k)});
                }
                y+=S(18);
            }
            if (options_.renderer=="vulkan") {
                // Native addition (not original): Vulkan upscaler, applied immediately like the fullscreen toggle.
                static constexpr std::array<const char*,6> upscalers{"Kapalı (çizim ölçeği)","FSR 3.1.4 Doğal AA","FSR 3.1.4 Kalite",
                    "FSR 3.1.4 Dengeli","FSR 3.1.4 Performans","FSR 3.1.4 Ultra Performans"};
                const auto mode=options_.upscaler=="fsr3"?1+static_cast<std::size_t>(fsr_quality(options_.fsr_mode)):0;
                ui_->text(left,y,"Ölçekleme"); y+=lh()+S(2);
                Flow u{left,right,left,y};
                flow(u,upscalers[mode],nullptr,-1,"upscaler-next");
                y=flow_end(u)+S(4);
                ui_->text(left,y,"FSR Keskinlik"); y+=lh()+S(2);
                const int cell=std::max(1,width/11),value=static_cast<int>(std::lround(options_.fsr_sharpness*10));
                for (int k=0;k<=10;++k) {
                    ui_->rect(left+k*cell,y,cell-S(1),S(10),k<=value?Rgba{200,170,90,235}:Rgba{40,44,48,235});
                    hits_.push_back({left+k*cell,y-S(3),cell,S(16),nullptr,-1,"fsr-sharp:"+std::to_string(k)});
                }
                y+=S(18);
                ui_->text(left,y,"Kare üretimi"); y+=lh()+S(2);
                Flow g{left,right,left,y};
                // Frame generation needs an FSR mode; without one the row is informational only.
                const bool fsr=options_.upscaler=="fsr3";
                flow(g,!fsr?"Kapalı (FSR gerekli)":options_.frame_generation?"Açık":"Kapalı",nullptr,-1,fsr?"framegen-toggle":"");
                y=flow_end(g)+S(4);
            }
            Flow f{left,right,left,y+S(4)};
            flow(f,"Tamam",nullptr,-1,"prefs-keep"); flow(f,"Vazgeç",nullptr,-1,"prefs-revert"); // original ✓/✗ icons
            flow(f,"Tam ekran / pencere",nullptr,-1,"fullscreen");
        } else if (id==win_help) {
            const bool images=!help_images_.empty();
            const int text_right=images?left+width/2-S(8):right;
            Flow f{left,text_right,left,y};
            flow(f,"‹ Önceki",nullptr,-1,"help-"); flow(f,"Sonraki ›",nullptr,-1,"help+");
            y=flow_end(f)+S(4);
            if (scene_.help.empty()) paragraph(left,y,text_right-left,bottom,"Özgün yardım kaynağı yüklenemedi.");
            else {
                const auto [first,last]=help_range();
                const auto& page=scene_.help[static_cast<std::size_t>(help_page_)];
                // Book popup: header and page n/m of the opened book (VOD 16:30 "Nasıl Yürünür" 2/2); F1 lists every book.
                const auto title=help_book_.empty()?page.book+" / "+page.header:page.header+"  "+std::to_string(help_page_-first+1)+"/"+std::to_string(last-first);
                ui_->text(left,y,fit(title,text_right-left),gold_rgba); y+=lh()+S(4);
                int index=0;
                for (const auto& paragraph_text:page.paragraphs) {
                    if (index++<scroll) continue;
                    y=paragraph(left,y,text_right-left,bottom,paragraph_text.text,original_rgba(paragraph_text.color))+S(4);
                    if (y>=bottom) break;
                }
            }
            if (images) {
                const auto& [key,image]=help_images_[static_cast<std::size_t>(help_image_page_)];
                const int ix=left+width/2+S(8);
                f={ix,right,ix,top};
                flow(f,"‹ Çizim",nullptr,-1,"help-image-"); flow(f,"Çizim ›",nullptr,-1,"help-image+");
                const int iy=flow_end(f)+S(4);
                const float scale=std::min(static_cast<float>(std::max(1,right-ix))/static_cast<float>(image.width),
                                           static_cast<float>(std::max(1,bottom-iy))/static_cast<float>(image.height));
                ui_->image(image,{static_cast<float>(ix),static_cast<float>(iy),static_cast<float>(image.width)*scale,
                    static_cast<float>(image.height)*scale},{{{0,0},{1,0},{0,1},{1,1}}});
            }
        }
    }
    void paint_center() {
        if (!flash_.empty()) centered(width_/2,height_/5,fit(flash_,width_/2),flash_color_);
        const int y=height_*2/5;
        if (disconnected_) {
            center_button(y,"Yeniden bağlan (R)",nullptr,"reconnect");
            center_button(y+lh()+S(16),"Çıkış",nullptr,"quit");
        } else if (dead()) {
            // Original 0041c2a0: "Canlanma Süresi (%d)" (remaining ms/1000) until the deadline, then "   Yeniden Canlan   ".
            const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(resurrect_at_-Clock::now()).count();
            if (ms<1) center_button(height_/9,"   Yeniden Canlan   ",Json{{"action","resurrect"}});
            else center_button(height_/9,"Canlanma Süresi ("+std::to_string(ms/1000)+")",nullptr);
        }
    }
    bool dead() const { return session_ && !disconnected_ && state_.contains("character") && character_state(state_).value("hp",1)<=0; }
    // ---- Floating stat text: original 0047c150 -> 0047bf80, names/colours 004b4800, stat handler 004a59d0. ----
    // Body-pick top above the feet. 00479fd0 gives the entity root the classid's classes.dat bounds (0043a417 table
    // 0x60acfc): half extents (boundx,boundy,boundz) centred at (0,boundy,0), so the box top is 2*boundy (PCs/townsfolk 4 ->
    // 8, rat 1 -> 2, Büyük Bronz 9 -> 18). Name tags and floaters use the 0047c530 anchor instead (tag_anchor).
    static constexpr float head_height=8; // no classid bounds record
    // Each attribute writes its own table column, so every axis keeps the last record that set it (0 = never set).
    std::array<float,3> class_bounds(unsigned source_class) const {
        std::array<float,3> half{};
        const auto id=std::to_string(source_class);
        for (const auto& r:scene_.actor_tables.source_records) if (r.class_id==id) {
            if (r.bound_x>0) half[0]=r.bound_x;
            if (r.bound_y>0) half[1]=r.bound_y;
            if (r.bound_z>0) half[2]=r.bound_z;
        }
        return half;
    }
    float actor_height(std::uint32_t id) const {
        const auto a=std::find_if(actors_.begin(),actors_.end(),[id](const auto& actor){return actor.id==id;});
        return a==actors_.end()?head_height:a->height;
    }
    // 00478710: each frame every line rises frame_ms·0.05 px (0x596f78), i.e. 50 px/s; 00479720 drops it after 2 s.
    // ponytail: the 0.5 s fade is a VOD estimate.
    float floater_offset(const Floater& f,Clock::time_point now) const {
        return f.start+50*s_*std::chrono::duration<float>(now-f.born).count();
    }
    // '+%.0f %s' (0x59707c) for gains, '%.0f %s' (0x59706c) otherwise.
    void float_text(std::uint32_t id,std::int64_t value,const char* name,std::uint32_t positive,std::uint32_t negative) {
        if (!value) return;
        std::string text=(value>0?"+":"")+std::to_string(value);
        if (*name) { text+=' '; text+=name; }
        float_line(id,std::move(text),value>0?positive:negative);
    }
    void float_line(std::uint32_t id,std::string text,std::uint32_t argb) {
        const auto now=Clock::now();
        float start=0;
        // 0047c03e: while the entity's newest text has risen less than its height − 2, the new one starts height + 2 below it.
        for (auto it=floaters_.rbegin();it!=floaters_.rend();++it) if (it->id==id) { const float shown=floater_offset(*it,now); if (shown<static_cast<float>(lh()-2)) start=shown-static_cast<float>(lh()+2); break; }
        if (floaters_.size()==64) floaters_.pop_front();
        floaters_.push_back({id,std::move(text),original_rgba(argb),now,start});
        world_dirty_=true;
    }
    // Called with the new state before hud_values_ records it. Shown for self, group members and monsters (0047c1ba).
    void stat_floaters(const Json& state) {
        if (!session_) return;
        const auto& cs=character_state(state);
        const auto number=[](const Json& j,const char* key)->std::int64_t { return j.contains(key) && j.at(key).is_number_integer()?j.at(key).get<std::int64_t>():0; };
        if (hud_values_.contains("hp") && cs.contains("hp")) {
            const auto self=session_->self;
            float_text(self,number(cs,"hp")-number(hud_values_,"hp"),"enerji",0xff22ff22,0xffff0000);
            float_text(self,number(cs,"mana")-number(hud_values_,"mana"),"kudret",0xff0000ff,0xffff6600);
            auto xp=number(cs,"xp")-number(hud_values_,"xp");
            if (number(cs,"level")>number(hud_values_,"level")) xp=number(hud_values_,"xp_interval")-number(hud_values_,"xp")+number(cs,"xp");
            float_text(self,xp,"tecrübe",0xffaa22dd,0xffff6600);
            float_text(self,number(cs,"currency")-number(hud_values_,"currency"),"para",0xffffffff,0xffff6600);
        }
        std::vector<std::pair<std::uint32_t,int>> seen;
        if (state.contains("entities")) for (const auto& e:state.at("entities")) {
            const auto id=e.value("id",0u); const int hp=e.value("hp",0);
            seen.emplace_back(id,hp);
            const auto old=std::find_if(floater_hp_.begin(),floater_hp_.end(),[id](const auto& p){return p.first==id;});
            if (old==floater_hp_.end() || old->second==hp || old->second==0) continue; // respawns are not stat deltas
            const int delta=hp-old->second;
            // 0047c246: monster damage is 0xffeeee00 yellow; its narrow 'enerji' reaches a wide '%s' and renders no name.
            if (e.value("kind",std::string{})=="enemy") float_text(id,delta,delta<0?"":"enerji",0xff22ff22,0xffeeee00);
            else if (std::find(group_members_.begin(),group_members_.end(),Json(id))!=group_members_.end()) float_text(id,delta,"enerji",0xff22ff22,0xffff0000);
        }
        floater_hp_=std::move(seen);
    }
    const ikv::RenderEntity* pose(std::uint32_t id) const {
        const auto found=std::find_if(poses_.begin(),poses_.end(),[id](const auto& p){return p.id==id;});
        return found==poses_.end()?nullptr:&*found;
    }
    // 00478710 runs in the name loop for every root that passed the frustum test (labels_; the own root is always
    // visible, 0047c820): line rect left = anchor x − width/2, top = anchor y − offset, at the unlifted 0047c530 anchor.
    void paint_floaters() {
        const auto now=Clock::now();
        for (const auto& f:floaters_) {
            const auto* p=pose(f.id);
            if (!p || (f.id!=session_->self && std::none_of(labels_.begin(),labels_.end(),[&](const Label& l){return l.id==f.id;}))) continue;
            const auto anchor=tag_anchor(*p);
            if (!anchor) continue;
            const float age=std::chrono::duration<float>(now-f.born).count();
            auto color=f.color; color[3]=static_cast<std::uint8_t>(static_cast<float>(color[3])*std::clamp((2-age)/.5f,0.f,1.f));
            const int y=static_cast<int>((*anchor)[1]-floater_offset(f,now));
            label(static_cast<int>((*anchor)[0])-ui_->text_width(f.text)/2,y,f.text,color);
        }
    }
    // Effect icons beside a unit frame as the original draws its per-actor list (0x479840 add, 0x478940/0x47aa80 remove; frames
    // 0x486370 own, 0x4845a0 party, 0x485110/0x485bf0 target): 20 px skills.dat icons of the caster's class, oldest first, 5 per
    // row at a 17 px pitch (3 px overlap), no timer text; a skill whose skills.dat hasicon is 0 adds none (0044ccf0).
    void paint_effects(const Json& list,float ox,float oy,std::size_t limit) {
        if (!list.is_array()) return;
        std::size_t i=0;
        for (const auto& effect:list) {
            if (i==limit) break;
            const auto* record=effect.contains("skill_source")?source_skill(Json{{"source",effect.at("skill_source")}}):nullptr;
            if (record && !record->hasicon) continue;
            const int icon=record?record->icon:skill_icon(effect.value("id",std::string{}));
            source_icon((ox+17.f*static_cast<float>(i%5))*s_,(oy+17.f*static_cast<float>(i/5))*s_,20*s_,icon<0?2:icon); // 2: no source record
            ++i;
        }
    }
    // ---- Harita Penceresi (H). Original 00477775: map element UVs are texel = 1024 - position*2/3 around the player,
    // 512 texels wide; zone art is the 180-degree-turned support grid. '(%d, %d)' prints those texels (0x596ed0),
    // ui\eye.dds is the view cone, '%d:%.2d' (004a4e2b) the clock. Frame: UI2 611,798 186x203, map hole 15,34 158x157. ----
    static constexpr float map_scale=2.f/3;
    void ensure_zone_map() {
        const unsigned zone=ikv::source_scene_descriptor(*scene_.source_registry_id).support_id;
        if (zone==zone_map_zone_) return;
        char name[16]; std::snprintf(name,sizeof(name),"zone%02u.dds",zone);
        const auto base=asset_base();
        zone_map_=read_ui_image(base/"ui"/name); // 0041cb00 ui\zone%.2d.dds
        if (eye_.mips.empty()) eye_=read_ui_image(base/"ui/eye.dds");
        zone_map_zone_=zone;
    }
    void paint_minimap() {
        map_rect_={};
        const auto* self=session_?pose(session_->self):nullptr;
        if (!map_open_ || !self || !scene_.source_registry_id) return;
        ensure_zone_map();
        const float fx=static_cast<float>(width_)-188*s_,fy=static_cast<float>(height_)-205*s_;
        ui_atlas(original_ui2_,fx,fy,0,0,186,203,611,798,186,203);
        const float ix=fx+15*s_,iy=fy+34*s_,iw=158*s_,ih=157*s_;
        const float cu=1024-self->x*map_scale,cv=1024-self->z*map_scale,half=256.f*static_cast<float>(map_zoom_);
        const float l=(cu-half)/2048,r=(cu+half)/2048,t=(cv-half)/2048,b=(cv+half)/2048;
        ui_->image(zone_map_,{ix,iy,iw,ih},{{{l,t},{r,t},{l,b},{r,b}}});
        // Map help colours: enemies FFFF3333, neutral NPCs FFFFFF00, players 77008000, selected FF33FFFF.
        for (const auto& p:poses_) {
            const Json* e=p.id==session_->self?nullptr:entity(p.id);
            if (!e || (e->value("kind",std::string{})=="enemy" && !e->value("alive",true))) continue;
            const auto kind=e->value("kind",std::string{});
            const float sx=ix+iw*(1024-p.x*map_scale-(cu-half))/(2*half),sy=iy+ih*(1024-p.z*map_scale-(cv-half))/(2*half);
            if (sx<ix+2*s_ || sy<iy+2*s_ || sx>ix+iw-2*s_ || sy>iy+ih-2*s_) continue;
            const int d=std::max(2,S(4));
            ui_->rect(static_cast<int>(sx)-d/2,static_cast<int>(sy)-d/2,d,d,
                original_rgba(p.id==target_?0xff33ffff:kind=="enemy"?0xffff3333:kind=="player"?0x77008000:kind=="summon"?0xff00ff00:0xffffff00));
        }
        // eye.dds apex is its centre; texture-up maps to the camera heading (sin yaw, cos yaw) in map space.
        const float phi=3.14159265f-renderer_->camera().yaw,c=std::cos(phi),sn=std::sin(phi);
        std::array<std::array<float,2>,4> uv{{{-.5f,-.5f},{.5f,-.5f},{-.5f,.5f},{.5f,.5f}}};
        for (auto& o:uv) o={.5f+c*o[0]+sn*o[1],.5f-sn*o[0]+c*o[1]};
        ui_->image(eye_,{ix,iy,iw,ih},uv);
        const int left=static_cast<int>(ix)+S(4);
        label(left,static_cast<int>(iy)+S(3),"("+std::to_string(1024-static_cast<int>(self->x*map_scale))+", "+
            std::to_string(1024-static_cast<int>(self->z*map_scale))+")");
        // Original thgf hour drives the minimap clock text (004a4c90 "%d:%.2d"); before it arrives, the zone minute.
        const float hour=scene_.lighting.source_hour>=0?scene_.lighting.source_hour:scene_.lighting.source_minute/60;
        char clock[16]; std::snprintf(clock,sizeof(clock),"%d:%.2d",static_cast<int>(hour),std::min(59,static_cast<int>(std::fmod(hour,1.f)*60)));
        label(left,static_cast<int>(iy+ih)-lh()-S(3),clock);
        const int x=static_cast<int>(fx),y=static_cast<int>(fy);
        map_rect_={x,y,S(186),S(203)};
        hits_.push_back({x,y,S(186),S(203),nullptr,-1,{}});
        hits_.push_back({x+S(146),y+S(7),S(16),S(16),nullptr,-1,"help-book:Map"}); // 004b5725 -> 004b36d0
        hits_.push_back({x+S(164),y+S(7),S(16),S(16),nullptr,-1,"map"});
        map_center_={static_cast<int>(cu),static_cast<int>(cv)}; map_painted_=Clock::now();
    }
    // Per-frame HUD upkeep before paint_hud: text expiry, map repaint, F4 camera lock, cursor.
    void hud_frame() {
        const auto now=Clock::now();
        const auto floating=floaters_.size();
        while (!floaters_.empty() && now-floaters_.front().born>=std::chrono::milliseconds(2000)) floaters_.pop_front(); // 00479720: 0x7d0 ms
        if (!floaters_.empty() || floaters_.size()!=floating) world_dirty_=true; // the frame after the last expiry clears it
        if (const auto* self=session_?pose(session_->self):nullptr) {
            // ponytail: CPU overlay repaints per moved map texel and 4x/s for dots; a GPU map quad if HUD cost shows up.
            const std::array<int,2> center{static_cast<int>(1024-self->x*map_scale),static_cast<int>(1024-self->z*map_scale)};
            if (map_open_ && (center!=map_center_ || now-map_painted_>=std::chrono::milliseconds(250))) hud_dirty_=true;
            // F4 'Kamerayı kilitler': keep the camera behind the character's heading.
            if (const auto* actor=camera_locked_?actor_state(session_->self):nullptr) { auto camera=renderer_->camera(); camera.yaw=actor->shown_yaw; renderer_->set_camera(camera); }
        }
        update_cursor();
    }
    // ---- Cursors: 0048edca loads ui\cursor_*.dds with hotspot 0,0; 004b6970 (after the 004b3ba0 hover pick) picks attack over
    // monsters, interact over <loc> entities (flags & 0xf == 3), talk over NPCs, grab over lootable entities and point otherwise. ----
    enum CursorKind { cursor_point,cursor_attack,cursor_talk,cursor_grab,cursor_interact };
    void update_cursor() {
        if (!cursors_loaded_) {
            cursors_loaded_=true;
            const std::array<const char*,5> names{"point","attack","talk","grab","interact"};
            for (std::size_t i=0;i<names.size();++i) {
                auto image=read_ui_image(asset_base()/"ui"/(std::string("cursor_")+names[i]+".dds"));
                if (SDL_Surface* surface=SDL_CreateSurfaceFrom(static_cast<int>(image.width),static_cast<int>(image.height),
                        SDL_PIXELFORMAT_RGBA32,image.rgba.data(),static_cast<int>(image.width*4))) {
                    cursors_[i]=SDL_CreateColorCursor(surface,0,0); SDL_DestroySurface(surface);
                }
            }
        }
        float mx=0,my=0; SDL_GetMouseState(&mx,&my);
        const auto [x,y]=pixel(mx,my);
        std::uint32_t id=0;
        const auto hit=std::find_if(hits_.rbegin(),hits_.rend(),[&](const Hit& h){return inside({h.x,h.y,h.w,h.h},x,y);});
        if (hit==hits_.rend()) id=label_at(x,y);
        else if (hit->value.starts_with("target:")) id=static_cast<std::uint32_t>(std::strtoul(hit->value.c_str()+7,nullptr,10));
        if (hit==hits_.rend() && !id && session_ && session_->self) { std::optional<std::array<float,3>> ground; id=entity_at(x,y,ground); }
        const Json* hovered=entity(id);
        hover_=hovered && hovered->value("kind",std::string{})=="enemy" && !hovered->value("alive",true) && !hovered->value("looted",true)?id-sack_offset:id;
        if (id>=sack_offset && id<2*sack_offset && !hovered) id+=sack_offset; // a sack shows its corpse's cursor
        int kind=cursor_point;
        if (const Json* e=session_ && id!=session_->self?entity(id):nullptr) {
            const auto k=e->value("kind",std::string{});
            if (k!="player" && relation(*e)==1) kind=e->value("alive",true)?cursor_attack:!e->value("looted",true) && e->value("owner",0u)==session_->self?cursor_grab:cursor_point;
            else if (k=="door" || k=="reset" || k=="anvil" || k=="node" || (k=="prop" && e->value("interactable",false))) kind=cursor_interact; // classes.dat <loc>: entity type 3 (004b6970)
            else if (k=="prop") kind=cursor_point; // non-interactable prop (flag, bird, fan)
            else if (k!="player" && k!="summon" && k!="enemy") kind=cursor_talk; // an own-clan banner gets the plain cursor
        }
        if (kind==cursor_ || !cursors_[static_cast<std::size_t>(kind)]) return;
        SDL_SetCursor(cursors_[static_cast<std::size_t>(kind)]); cursor_=kind;
        if (std::getenv("IKV_HUD_TRACE")) std::cerr<<"HUD cursor="<<kind<<" entity="<<id<<" at="<<x<<','<<y<<'\n';
    }
    // ---- Loading screen (VOD 02:12): 00472b6e srand(time) + ui\loading%d.jpg (rand()%12+1; 1024x768 art in a 1024² JPEG),
    // ui\ikvlogo.dds, load_anim\gobi0535..0574 (0048ffb0), UI1 'Yükleniyor' 3,952 92x14 over the bar 3,992 313x29. ----
    static ikv::assets::DdsImage read_jpeg_image(const std::filesystem::path& path) {
        const auto size=std::filesystem::file_size(path);
        if (size<4 || size>8*1024*1024) throw std::runtime_error("original JPEG exceeds size limit");
        std::ifstream file(path,std::ios::binary);
        std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
        if (!file.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(size))) throw std::runtime_error("original JPEG read failed");
        const std::unique_ptr<void,decltype(&tj3Destroy)> decoder(tj3Init(TJINIT_DECOMPRESS),&tj3Destroy);
        if (!decoder || tj3DecompressHeader(decoder.get(),bytes.data(),bytes.size())) throw std::runtime_error("original JPEG header invalid");
        const int w=tj3Get(decoder.get(),TJPARAM_JPEGWIDTH),h=tj3Get(decoder.get(),TJPARAM_JPEGHEIGHT);
        if (w<=0 || h<=0 || w>4096 || h>4096) throw std::runtime_error("original JPEG dimensions exceed limit");
        const auto bytes_out=static_cast<std::size_t>(w)*static_cast<std::size_t>(h)*4;
        ikv::assets::DdsImage image{static_cast<std::uint32_t>(w),static_cast<std::uint32_t>(h),std::vector<std::uint8_t>(bytes_out),{}};
        if (tj3Decompress8(decoder.get(),bytes.data(),bytes.size(),image.rgba.data(),w*4,TJPF_RGBA)) throw std::runtime_error("original JPEG decode failed");
        image.mips.push_back({image.width,image.height,0,bytes_out});
        return image;
    }
    void begin_loading() {
        if (loading_) return;
        const auto base=asset_base();
        loading_art_.art=read_jpeg_image(base/"ui"/("loading"+std::to_string(1+SDL_rand(12))+".jpg"));
        if (loading_art_.frames.empty()) {
            loading_art_.logo=read_ui_image(base/"ui/ikvlogo.dds");
            for (int frame=535;frame<=574;++frame) loading_art_.frames.push_back(read_ui_image(base/"ui/load_anim"/("gobi0"+std::to_string(frame)+".DDS")));
        }
        if (!original_ui_cached_) init_original_ui(); // UI1 'Yükleniyor' bar
        loading_=true; loading_progress_=0; loading_painted_={};
        paint_loading();
    }
    void loading_stage(float progress) { if (loading_) { loading_progress_=progress; loading_painted_={}; paint_loading(); } }
    void paint_loading() {
        const auto now=Clock::now();
        int w=0,h=0;
        if (!renderer_ || now-loading_painted_<std::chrono::milliseconds(50) || !SDL_GetWindowSizeInPixels(window_,&w,&h) || w<=0 || h<=0) return;
        loading_painted_=now;
        if (w!=width_ || h!=height_) { width_=w; height_=h; renderer_->resize(w,h); }
        s_=std::clamp(static_cast<float>(height_)/1080.f,.6f,2.f);
        if (const int px=std::max(11,static_cast<int>(std::lround(16*s_))); !ui_ || px!=ui_px_) {
            ui_=std::make_unique<ikv::ClientUI>(asset_base()/"fonts/verdana.ttf",px); ui_px_=px;
        }
        const float fw=static_cast<float>(width_),fh=static_cast<float>(height_);
        static constexpr std::array<std::array<float,2>,4> whole{{{0,0},{1,0},{0,1},{1,1}}};
        ui_->begin(width_,height_);
        ui_->image(loading_art_.art,{0,0,fw,fh},{{{0,0},{1,0},{0,.75f},{1,.75f}}});
        ui_->image(loading_art_.logo,{-34*s_,14*s_,256*s_,64*s_},whole);
        // ponytail: 30 fps mascot clock; 0048ffb0 advances one frame per loading paint.
        ui_->image(loading_art_.frames[SDL_GetTicks()/33%loading_art_.frames.size()],{fw-128*s_,fh-128*s_,128*s_,128*s_},whole);
        if (loading_progress_<.45f) loading_progress_+=.005f; // ponytail: ~0.1/s creep until the CPU scene load reports its .5 stage
        if (original_ui_cached_) {
            const float bx=(fw-313*s_)/2,by=fh-81*s_,p=std::clamp(loading_progress_,0.f,1.f);
            atlas_image(original_ui1_,(fw-92*s_)/2,by-30*s_,92*s_,14*s_,3,952,92,14);
            atlas_image(original_ui1_,bx,by,313*s_,29*s_,3,992,313,29);
            atlas_image(original_ui1_,bx+17*s_,by+4*s_,274*p*s_,22*s_,5,966,274*p,22);
        }
        renderer_->set_overlay(ui_->overlay());
        renderer_->render(loading_scene_,{},0,0);
        hud_dirty_=true;
    }
    // ---- Chat: original channel table 0040c3a0 (+0xec, stride 0x24) and helps.xml 'Mesaj Kanalları'. Klan ('/k') is the
    // Arzın Çocukları/Lodos clan channel, Lonca (FF33FF33) the player guild; '/l' for Lonca is NONCANONICAL. ----
    struct ChatChannel { const char* prefix; std::uint32_t argb; const char* wire; char command; bool once; };
    static constexpr std::array<ChatChannel,8> chat_channels{{
        {"Bölge: ",0xffff8033,"zone",'b',true},{"Klan: ",0xff33ffff,"clan",'k',false},{"Grup: ",0xffff3333,"group",'g',false},
        {"Söyle: ",0xffffffff,"local",'s',false},{"Rol yap: ",0xff60a0ff,"roleplay",'r',true},{"Fısılda: ",0xffff33ff,"private",'f',true},
        {"Lonca: ",0xff33ff33,"guild",'l',false},{"",0xffffff00,"",0,false}}};
    static constexpr int chat_say=3,chat_whisper=5,chat_system=7;
    static_assert(chat_system+1==static_cast<int>(chat_channels.size()) && chat_system==std::tuple_size_v<decltype(chat_hidden_)>);
    // Fısıldama, Rol yapma and Bölge stay active for one message, then the previous channel returns.
    void select_channel(int channel) { if (!chat_channels[static_cast<std::size_t>(channel_)].once) chat_previous_=channel_; channel_=channel; }
    void whisper(std::uint32_t id,std::string name) { if (!id) return; whisper_to_=id; whisper_name_=std::move(name); select_channel(chat_whisper); }
    // '/s', '/r', '/g', '/k', '/b' + space switch channels; '/f name' + space picks a whisper recipient (by visible name).
    void chat_command() {
        if (input_.size()<3 || input_[0]!='/' || input_.back()!=' ') return;
        if (input_.size()==3) for (int i=0;i<chat_system;++i) if (i!=chat_whisper && input_[1]==chat_channels[static_cast<std::size_t>(i)].command) { select_channel(i); input_.clear(); return; }
        if (!input_.starts_with("/f ") || input_.size()<5) return;
        const auto name=input_.substr(3,input_.size()-4);
        if (name.find(' ')!=std::string::npos) return;
        input_.clear();
        for (const auto& e:hud_entities_) if (e.contains("class_id") && e.value("name",std::string{})==name) { whisper(e.value("id",0u),name); return; }
        notice("Görüş alanında bu isimde oyuncu yok: "+name); // native private chat addresses entity ids
    }
    // Window tooltip (0048b6a0 lines into the manager's box, placed by 0048de14): width = widest line + 6, height = 12 +
    // line height·1.2 per line, at pointer + (6, 20) kept 16 px inside the screen. ponytail: no hover delay (+0xac tick).
    void paint_tip() {
        if (tip_.empty()) return;
        float mouse_x=0,mouse_y=0; SDL_GetMouseState(&mouse_x,&mouse_y);
        const auto [mx,my]=pixel(mouse_x,mouse_y);
        int w=0; for (const auto& [text,color]:tip_) w=std::max(w,ui_->text_width(text));
        const float pitch=static_cast<float>(lh())*1.2f;
        w+=S(6); const int h=S(12)+static_cast<int>(pitch*static_cast<float>(tip_.size()));
        const int x=std::min(static_cast<int>(mx)+S(6),width_-w-S(16)),y=std::min(static_cast<int>(my)+S(20),height_-h-S(16));
        ui_->rect(x,y,w,h,{8,8,12,230}); outline(x,y,w,h,{150,130,90,255});
        for (std::size_t i=0;i<tip_.size();++i) label(x+S(3),y+S(6)+static_cast<int>(pitch*static_cast<float>(i)),tip_[i].first,tip_[i].second);
    }
    // Tooltip of the window under the pointer: rect in screen pixels.
    bool hovered(int x,int y,int w,int h) const {
        float mouse_x=0,mouse_y=0; SDL_GetMouseState(&mouse_x,&mouse_y);
        const auto [mx,my]=pixel(mouse_x,mouse_y);
        return inside({x,y,w,h},mx,my);
    }
    // Name labels and floaters are world-anchored and beneath every HUD window (painted first), so they live on ClientUI's
    // world layer: when only they moved, paint_world alone repaints and uploads their old and new boxes. Their hits stay the
    // lowest-priority prefix of hits_.
    void paint_world() {
        hits_.erase(hits_.begin(),hits_.begin()+static_cast<std::ptrdiff_t>(label_hits_));
        const auto rest=hits_.size();
        ui_->begin_world(); paint_labels(); paint_floaters(); ui_->end_world();
        std::rotate(hits_.begin(),hits_.begin()+static_cast<std::ptrdiff_t>(rest),hits_.end());
        label_hits_=hits_.size()-rest; world_dirty_=false;
    }
    void present_hud() { if (const auto changed=ui_->compose(); !changed.empty()) renderer_->set_overlay(ui_->overlay(),changed); }
    void paint_hud() {
        ui_->begin(width_,height_); hits_.clear(); label_hits_=0; drags_.clear(); window_rects_.clear(); tip_.clear();
        const auto& st=character_state(state_);
        std::string name="Karakter doğrulanıyor";
        if (session_ && state_.contains("character")) name=state_.at("character").value("name",name);
        paint_world();
        paint_frames(st,name);
        paint_chat();
        paint_bar();
        paint_minimap();
        paint_windows(st);
        paint_dialogue(st);
        paint_loot();
        paint_center();
        paint_system();
        paint_tip();
        if (!chat_link_open_.is_null()) paint_item_tooltip(chat_link_x_,chat_link_y_,chat_link_open_.at("entry"),st,chat_link_open_.at("state"));
        if (dragging_) paint_slot_content(drag_x_-14*s_,drag_y_-14*s_,28*s_,drag_.contains("slot")?quickbar().at(drag_.at("slot").get<std::size_t>()):drag_); // icon under the cursor
        if (std::getenv("IKV_HUD_TRACE")) { // drag sources for capture scripts (skill icons, bag items, quickbar slots), on change
            Json traced=Json::array(); for (const auto& [r,payload]:drags_) traced.push_back({r[0],r[1],r[2],r[3],payload});
            if (auto text=traced.dump(); text!=drag_trace_) { drag_trace_=std::move(text); std::cerr<<"HUD drags="<<drag_trace_<<'\n'; }
        }
        present_hud(); hud_dirty_=false;
    }
    void source_icon(float x,float y,float size,int selector,std::uint32_t tint=0xffffffff) {
        if (selector<0) return; // Source selector UINT32_MAX has an empty UV extent.
        const int n=selector<272?selector:selector-272;
        const float u=selector<272?32.f*static_cast<float>(n&15):320.f+32.f*static_cast<float>(n%22);
        const float v=selector<272?32.f*static_cast<float>(n>>4):768.f+32.f*static_cast<float>(n/22);
        atlas_image(original_ui1_,x,y,size,size,u,v,32,32,tint);
    }
    void cache_original_ui_state() {
        if (!session_ || !scene_.actor_tables.loaded || !state_.contains("catalogs")) return;
        const auto& catalogs=state_.at("catalogs");
        if (!catalogs.contains("items")) return;
        for (const auto& item:catalogs.at("items")) {
            if (!item.contains("source_uid")) continue;
            const auto uid=item.at("source_uid").get<unsigned>();
            if (std::any_of(source_item_icons_.begin(),source_item_icons_.end(),[&](const auto& entry){return entry.first==uid;})) continue;
            const auto& record=ikv::source_actor_record(scene_,uid);
            // Source potion icons depend on original instance effects, not native healing amounts.
            source_item_icons_.emplace_back(uid,record.object_type=="potion"?-1:record.icon);
            hud_dirty_=true;
        }
    }
    ActorState* actor_state(std::uint32_t id) {
        const auto found=std::find_if(actors_.begin(),actors_.end(),[id](const auto& actor){return actor.id==id;});
        return found==actors_.end()?nullptr:&*found;
    }
    // Actor models are assembled on a worker (prepare_* against actor_context_) and installed on a later frame: an actor
    // entering view mid-session appears when ready instead of stalling the frame loop. Zone entry still waits behind the
    // loading screen, as the original zone load does. Each request carries a serial; a result whose actor despawned or was
    // re-dressed meanwhile is dropped.
    void attach_models(const PendingMutation& mutation,bool loading) {
        struct ModelChange {
            std::shared_ptr<const Json> data;
            std::string member;
            ikv::SourceAppearance source;
        };
        std::vector<ModelChange> changes;
        auto removed=mutation.removed;
        if (mutation.reset) for (const auto& actor:actors_)
            if (std::none_of(mutation.actors.begin(),mutation.actors.end(),[&](const auto& row){return row->at("id")==actor.id;}))
                removed.push_back(actor.id);
        for (const auto& row:mutation.actors) {
            const auto& data=*row;
            const auto id=data.at("id").get<std::uint32_t>();
            const bool pc=data.at("pc").get<bool>();
            const auto& appearance=data.at("appearance");
            std::string member;
            const auto mapped=model_map_.find("id:"+std::to_string(id));
            if (mapped!=model_map_.end() && !mapped->get_ref<const std::string&>().empty()) member=mapped->get<std::string>();
            if (member.empty() && appearance.contains("model_member")) member=appearance.at("model_member").get<std::string>();
            const auto source_class=pc?2*data.at("class_id").get<unsigned>()+appearance.value("sex",1u):data.at("source_class").get<unsigned>();
            const auto& equipment=data.at("equipment");
            const auto* actor=actor_state(id);
            if (actor && actor->binding.at("member")==member && actor->binding.at("appearance")==appearance &&
                actor->binding.at("equipment")==equipment && actor->source_class==source_class &&
                actor->binding.at("uid")==data.value("source_uid",0u)) continue;
            ModelChange change; change.data=row; change.member=std::move(member); change.source.source_class=source_class;
            if (change.member.empty()) {
                auto& source=change.source;
                if (pc) {
                    source.face=appearance.value("face",0u); source.hair=appearance.value("hair",0u);
                    source.hair_color=appearance.value("hair_color",0u); source.beard=appearance.value("beard",0u);
                    source.beard_color=appearance.value("beard_color",0u);
                }
                source.equipment.reserve(equipment.size()); // NPCs: classes.dat rhweaponid/lhweaponid, server-sent like a PC's
                for (const auto& item:equipment) source.equipment.push_back({item.at("slot").get<unsigned>(),item.at("source_uid").get<unsigned>()});
            }
            if (changes.empty()) changes.reserve(mutation.actors.size());
            changes.push_back(std::move(change));
        }
        if (changes.empty() && removed.empty()) return;
        for (const auto id:removed) ikv::detach_source_actor(scene_,id);
        std::erase_if(actors_,[&](const auto& actor){return std::find(removed.begin(),removed.end(),actor.id)!=removed.end();});
        for (auto& change:changes) {
            const auto& data=*change.data;
            const auto id=data.at("id").get<std::uint32_t>();
            auto* actor=actor_state(id);
            if (!actor) { actors_.push_back({}); actor=&actors_.back(); actor->id=id; }
            actor->binding={{"member",change.member},{"appearance",data.at("appearance")},
                {"equipment",data.at("equipment")},{"uid",data.value("source_uid",0u)}};
            actor->source_class=change.source.source_class; actor->assembly=++assembly_serial_;
            // Bounds, weapon and stance apply at once (labels, picking, locomotion); the shadow waits for the new model.
            if (scene_.actor_tables.loaded) describe_actor(*actor,data,actor_controller(id)!=nullptr);
            assembly_queue_.push_back({actor->assembly,std::move(change.data),std::move(change.member),std::move(change.source)});
        }
        if (!loading && scene_.actor_tables.loaded) return; // poll_assembly starts the worker
        if (assembly_.valid()) install_models(load_cpu([&]{return assembly_.get();}));
        // ponytail: whole-cache reset at a zone entry past half the 4096-model budget; LRU if long sessions thrash it.
        if (actor_context_.model_resources.size()>2048) actor_context_.model_resources.clear();
        install_models(load_cpu([&,requests=std::move(assembly_queue_),resident=resident_textures()]{return assemble(requests,resident);}));
        assembly_queue_.clear();
    }
    // Each frame after the mutations: install a finished batch, then start the next one.
    void poll_assembly() {
        if (assembly_.valid() && assembly_.wait_for(std::chrono::seconds(0))==std::future_status::ready) install_models(assembly_.get());
        if (assembly_.valid() || assembly_queue_.empty()) return;
        assembly_=std::async(std::launch::async,[this,requests=std::move(assembly_queue_),resident=resident_textures()]{return assemble(requests,resident);});
        assembly_queue_.clear();
    }
    ikv::Scene resident_textures() const { // names only: the worker decodes just what the scene lacks
        ikv::Scene resident; resident.textures.resize(scene_.textures.size());
        for (std::size_t i=0;i<scene_.textures.size();++i) resident.textures[i].name=scene_.textures[i].name;
        return resident;
    }
    // Worker side: touches actor_context_ and the requests only.
    AssemblyBatch assemble(const std::vector<AssemblyRequest>& requests,ikv::Scene resident) {
        AssemblyBatch batch; batch.results.reserve(requests.size());
        const auto& root=options_.assets;
        for (const auto& request:requests) {
            auto& result=batch.results.emplace_back(); result.serial=request.serial; result.data=request.data;
            const auto id=request.data->at("id").get<std::uint32_t>();
            const bool pc=request.data->at("pc").get<bool>();
            try {
                if (!request.member.empty()) result.actor=ikv::prepare_entity_model(actor_context_,root,request.member,id,&resident);
                else if (pc) result.actor=ikv::prepare_source_actor(actor_context_,root,request.source,id,&resident);
                else result.actor=ikv::prepare_source_npc(actor_context_,root,request.data->value("source_uid",0u),id,request.source.equipment,&resident);
            } catch (const std::exception& error) {
                if (!request.member.empty()) result.fatal=std::current_exception(); // an explicit model override must exist
                else result.error=error.what();
            }
            // One decoded copy per batch: later requests see it as resident, install adds the pool before any model.
            if (result.actor) for (auto& texture:result.actor->model.textures) {
                resident.textures.emplace_back().name=texture.name;
                batch.textures.push_back(std::move(texture));
            }
            if (result.actor) result.actor->model.textures.clear();
        }
        return batch;
    }
    void install_models(AssemblyBatch batch) {
        for (auto& message:actor_context_.diagnostics) scene_.diagnostics.push_back(std::move(message));
        actor_context_.diagnostics.clear();
        if (!scene_.actor_tables.loaded && actor_context_.actor_tables.loaded) scene_.actor_tables=actor_context_.actor_tables;
        if (std::none_of(batch.results.begin(),batch.results.end(),[&](const auto& r){const auto* a=actor_state(r.data->at("id").template get<std::uint32_t>());return a && a->assembly==r.serial;})) return;
        for (auto& texture:batch.textures)
            if (std::none_of(scene_.textures.begin(),scene_.textures.end(),[&](const auto& old){return old.name==texture.name;})) scene_.textures.push_back(std::move(texture));
        for (auto& result:batch.results) {
            if (result.fatal) std::rethrow_exception(result.fatal);
            const auto& data=*result.data;
            const auto id=data.at("id").get<std::uint32_t>();
            auto* actor=actor_state(id);
            if (!actor || actor->assembly!=result.serial) continue;
            const bool fresh=!actor_controller(id);
            if (result.actor) { ikv::install_actor(scene_,std::move(*result.actor)); if (!fresh) actor->locomotion_dirty=true; }
            // An original record the source client could not assemble either (e.g. classes.dat uid 900/901 templates) leaves
            // only the entity's name label; it must not end the session.
            else { const bool pc=data.at("pc").get<bool>(); std::cerr<<"actor "<<id<<(pc?" class ":" source uid ")<<(pc?actor->source_class:data.value("source_uid",0u))<<" not rendered: "<<result.error<<'\n'; }
            describe_actor(*actor,data,result.actor.has_value());
            // A rebuilt model starts in its bind pose: a body re-dressed while dead (equipment/durability change) keeps lying.
            if (!actor->alive && !actor->node) lie(*actor);
            // Original hcpe handler 004a6720 starts the own character on clip 27 (Idle_LookAround) when it enters the world.
            else if (!actor->node && session_ && id==session_->self && actor->hp<0 && is_pc(*actor)) {
                if (const auto clip=pc_clip(*actor,"Idle_LookAround"); clip!=UINT32_MAX) actor_action(*actor,clip);
            }
            // A player already sitting/lying (server "posture", the held RP clip) is shown in it on arrival or rebuild.
            else if (const unsigned held=data.value("posture",0u); held && !actor->node) actor_action(*actor,held,true);
            // Original spawns carried the server's clip id (004a4270 -> 0047bea0). Where the generic startup clip 2 is the
            // cliplist's Death (Er Meydanı sancakları, Karkum, Erg Küresi: animation.dat state 8) a living one waits, not lies flat.
            else if (!actor->node && pick_clip(*actor,[](const ikv::ActorClip& c){return c.id==2 && c.state==8;})!=UINT32_MAX) {
                if (const auto clip=state_clip(*actor,3); clip!=UINT32_MAX) actor_action(*actor,clip);
            }
            // A node whose life arrived while it assembled gets actor_life's initial clip now.
            if (actor->node && fresh && actor->hp>=0)
                if (const auto clip=pick_clip(*actor,[&](const ikv::ActorClip& c){return c.id==(actor->alive?1u:3u);}); clip!=UINT32_MAX) actor_action(*actor,clip,false,!actor->alive);
        }
    }
    void describe_actor(ActorState& actor,const Json& data,bool assembled) {
        actor.bounds=class_bounds(actor.source_class);
        actor.height=actor.bounds[1]>0?2*actor.bounds[1]:head_height; hud_dirty_=true;
        // 00473e69 copies the class record's shadowtype (absent = empty = no shadow; PC body roots carry none).
        actor.shadow.clear();
        if (const auto uid=data.value("source_uid",0u); uid && !data.at("pc").get<bool>() && assembled)
            for (const auto& record:scene_.actor_tables.source_records) if (record.uid==uid && (record.tag=="npc" || record.tag=="monster")) { actor.shadow=record.shadow; break; }
        actor.node=data.value("kind",std::string{})=="node";
        actor.weapon.clear(); actor.pose=2;
        for (const auto& item:data.at("equipment")) if (item.at("slot")==11)
            for (const auto& record:scene_.actor_tables.source_records)
                if (record.tag=="object" && record.uid==item.at("source_uid").get<unsigned>()) { actor.weapon=record.object_type; actor.pose=record.pose>0?static_cast<unsigned>(record.pose):6; break; }
    }
    void actor_action(ActorState& actor,unsigned clip,bool restart=false,bool terminal=false) {
        if (!actor.binding.at("member").get_ref<const std::string&>().empty()) return; // Explicit inspection override has no canonical action policy.
        if (!actor_controller(actor.id)) return; // Unassembled actor (attach_models logged why): label only, nothing to animate.
        ikv::set_actor_action(scene_,actor.id,clip,terminal?0.f:.25f,restart,terminal);
    }
    static bool is_pc(const ActorState& actor) { return actor.source_class>=2 && actor.source_class<=7; }
    void actor_life(std::uint32_t id,int hp,bool alive) {
        auto* actor=actor_state(id);
        if (!actor) return;
        const bool initial_life=actor->hp<0;
        const bool damaged=actor->hp>=0 && hp<actor->hp;
        actor->hp=hp;
        if (actor->node) { // gathering node: loc cliplist maden_Wait (1) while harvestable, picked frames (3) while regrowing
            if (actor->alive!=alive || initial_life) {
                actor->alive=alive;
                if (const auto clip=pick_clip(*actor,[&](const ikv::ActorClip& c){return c.id==(alive?1u:3u);}); clip!=UINT32_MAX) actor_action(*actor,clip,false,initial_life&&!alive);
            }
            return;
        }
        // Original rtea handler 004a59d0 spawns the fixed "kan" (blood) system on every HP decrease.
        if (damaged && effects_) effects_->trigger("kan",id);
        // NONCANONICAL: original resurrection visual is unrecovered; reuse the original "heal" effect.
        if (!initial_life && alive && !actor->alive && effects_) effects_->trigger("heal",id);
        if (actor->alive!=alive) {
            actor->alive=alive; actor->moving=false; actor->locomotion_dirty=false; actor->samples=0; // Respawns teleport, never slide.
            if (!alive) actor->died_at=Clock::now();
            if (!alive && initial_life) { lie(*actor); return; }
            const auto clip=!alive?death_clip(*actor):is_pc(*actor)?pc_clip(*actor,"Resurrection"):state_clip(*actor,3);
            if (clip!=UINT32_MAX) actor_action(*actor,clip);
        } else if (alive && damaged) {
            const auto clip=is_pc(*actor)?pc_damage(*actor):state_clip(*actor,7,actor->swing++);
            if (clip!=UINT32_MAX) actor_action(*actor,clip);
        }
    }
    // variant-th clip (wrapping) of the actor's own animation.dat cliplist that satisfies pred; UINT32_MAX if none.
    template<class Pred> std::uint32_t pick_clip(const ActorState& actor,Pred&& pred,unsigned variant=0) {
        const auto* controller=actor_controller(actor.id);
        if (!controller || controller->library>=scene_.actor_tables.libraries.size()) return UINT32_MAX;
        const auto& clips=scene_.actor_tables.libraries[controller->library].clips;
        const auto count=static_cast<unsigned>(std::count_if(clips.begin(),clips.end(),pred));
        if (!count) return UINT32_MAX;
        for (variant%=count; const auto& c:clips) if (pred(c) && !variant--) return c.id;
        return UINT32_MAX;
    }
    // NPC/monster cliplists tag clips with an animation.dat action class (`state`: 1 run, 2 walk, 3 wait, 5 attack,
    // 7 damage, 8 death). Selecting the variant-th clip of a class works for every monster family [INFERENCE from names].
    std::uint32_t state_clip(const ActorState& actor,std::uint32_t state,unsigned variant=0) {
        const auto clip=pick_clip(actor,[&](const ikv::ActorClip& c){return c.state==state;},variant);
        return clip==UINT32_MAX && state==1?state_clip(actor,2,variant):clip; // walkers without a run clip
    }
    // The server sent which locomotion clip an NPC played (004a4270). NONCANONICAL: below the midpoint of its walk (state 2)
    // and run (state 1) forwardspeed (×1000 = wire units/s) it walks, so a classes.dat idlewalktend stroll is not a sprint.
    std::uint32_t npc_gait(const ActorState& actor,float speed) {
        const auto* controller=actor_controller(actor.id);
        if (!controller || controller->library>=scene_.actor_tables.libraries.size()) return UINT32_MAX;
        const ikv::ActorClip *walk=nullptr,*run=nullptr;
        for (const auto& c:scene_.actor_tables.libraries[controller->library].clips) { if (c.state==2 && !walk) walk=&c; if (c.state==1 && !run) run=&c; }
        if (!walk || !run) return run?run->id:walk?walk->id:UINT32_MAX;
        return speed<(walk->forward_speed+run->forward_speed)*500?walk->id:run->id;
    }
    // PC cliplists carry no `state`; actions resolve by authored clip name (prefix…suffix). The official server sent the
    // clip id (004a4270 -> 0047bea0), so which named clip fits a moment is NONCANONICAL; names and stance are original.
    std::uint32_t pc_clip(const ActorState& actor,std::string_view prefix,std::string_view suffix={},unsigned variant=0) {
        return pick_clip(actor,[&](const ikv::ActorClip& c){return c.name.starts_with(prefix) && c.name.ends_with(suffix);},variant);
    }
    // Wait = the weapon object's `pose` clip; run = the looping Run_* clip whose authored jumptoid returns to that wait.
    std::uint32_t pc_locomotion(const ActorState& actor,bool moving) {
        const auto wait=pick_clip(actor,[&](const ikv::ActorClip& c){return c.id==actor.pose && c.loops;})!=UINT32_MAX?actor.pose:2u;
        if (!moving) return wait;
        const auto run=pick_clip(actor,[&](const ikv::ActorClip& c){return c.loops && c.next_id==wait && c.name.starts_with("Run_");});
        return run!=UINT32_MAX?run:pc_clip(actor,"Run_BareHand");
    }
    // Swing and flinch families by the equipped weapon class: bare hands (pose 2: none, or Pençe claws) Punch_*/Kick_*
    // (weapon\barehand\1..6.raw), ranged Shoot*_FireArms, staff Hit_Wooden_*, heavy Hit_BluntWeapons_*, light
    // Hit_SharpWeapons_*; each actor cycles its family. Their authored jumptoid returns to the class guard (11/29/30).
    std::uint32_t pc_attack(ActorState& actor) {
        const auto& w=actor.weapon; const auto v=actor.swing++;
        if (actor.pose==2) return pick_clip(actor,[](const ikv::ActorClip& c){return c.name.starts_with("Punch_") || c.name.starts_with("Kick_");},v);
        return w=="weaponranged"?pc_clip(actor,"Shoot","_FireArms",v):w=="weaponstaff"?pc_clip(actor,"Hit_Wooden","",v):
            w=="weaponheavy"?pc_clip(actor,"Hit_BluntWeapons","",v):pc_clip(actor,"Hit_SharpWeapons","",v);
    }
    std::uint32_t pc_damage(ActorState& actor) {
        const auto v=actor.swing++;
        return actor.pose==2?pc_clip(actor,"Damage_","_BareHand",v):
            actor.weapon=="weaponranged"?pc_clip(actor,"Damage_FireArms"):pc_clip(actor,"Damage_","_Armed",v);
    }
    std::uint32_t pc_guard(const ActorState& actor) {
        return pc_clip(actor,actor.pose==2?"Guard_BareHand":actor.weapon=="weaponranged"?"Guard_FireArms":"Guard_SharpWeapons");
    }
    std::uint32_t death_clip(const ActorState& actor) { return is_pc(actor)?pc_clip(actor,"Death"):state_clip(actor,8); }
    // 004a4190/004a4710: an entity whose HP stat is at or below 0 gets clip 0, the authored last frame of its death
    // (228 of 231 cliplists with both). Lists without a clip 0 hold the end of their death clip instead.
    void lie(ActorState& actor) {
        const auto clip=pick_clip(actor,[](const ikv::ActorClip& c){return c.id==0;})!=UINT32_MAX?0u:death_clip(actor);
        if (clip!=UINT32_MAX) actor_action(actor,clip,false,true);
    }
    ikv::ActorAnimation* actor_controller(std::uint32_t id) {
        const auto assembly=std::find_if(scene_.actor_assemblies.begin(),scene_.actor_assemblies.end(),[id](const auto& value){return value.entity_id==id;});
        if (assembly==scene_.actor_assemblies.end() || assembly->controller>=scene_.actors.size()) return nullptr;
        return &scene_.actors[assembly->controller];
    }
    const ikv::ActorClip* actor_clip(const ikv::ActorAnimation* controller) const {
        if (!controller || controller->library>=scene_.actor_tables.libraries.size()) return nullptr;
        const auto& clips=scene_.actor_tables.libraries[controller->library].clips;
        return controller->current_clip<clips.size()?&clips[controller->current_clip]:nullptr;
    }
    static constexpr auto tick_period=std::chrono::milliseconds(50); // World enforces 20 Hz.
    static constexpr double interpolation_ticks=2,late_gait_ticks=2;
    static constexpr std::int32_t teleport_distance=20000;
    // Records one authoritative position per server tick; draw() renders the history interpolation_ticks behind.
    void move_actor(const ikv::Player& position,std::uint32_t tick) {
        auto* actor=actor_state(position.id);
        if (!actor) return;
        auto& h=actor->history; auto& n=actor->samples;
        if (n && tick==h[n-1].tick) { h[n-1]={tick,position.x,position.y}; return; }
        // A new session's older ticks and teleports/respawns restart the history instead of sliding across the map.
        if (n && (tick<h[n-1].tick || std::abs(position.x-h[n-1].x)>teleport_distance || std::abs(position.y-h[n-1].z)>teleport_distance)) n=0;
        if (n==h.size()) { std::move(h.begin()+1,h.end(),h.begin()); --n; }
        h[n++]={tick,position.x,position.y};
        if (!tick_epoch_ || tick>epoch_tick_) {
            // Track the least-delayed arrival; creep later slowly so clock drift cannot starve the buffer.
            const auto arrival=Clock::now()-tick_period*tick;
            if (!tick_epoch_ || arrival<*tick_epoch_ || arrival-*tick_epoch_>std::chrono::milliseconds(250)) tick_epoch_=arrival;
            else *tick_epoch_+=(arrival-*tick_epoch_)/16;
            epoch_tick_=tick;
        }
    }
    // Measured planted-foot ground speed of the original locomotion clips in wire units/s
    // (analysis/porting-wave1/Motion/README.md): PC Run_* clips 1/5 and rat Fare_Walk clip 4.
    static float stride_speed(const ActorState& actor) {
        if (actor.source_class>=2 && actor.source_class<=7) return 14000;
        return actor.source_class==9 ? 2200 : 0; // other monsters: unmeasured, clip clock stays authored
    }
    // Original 0047aa80 plays clips at the authored 30 fps. NONCANONICAL: when the authoritative ground
    // speed differs from the clip stride (server speed, buffs, catch-up ticks) the locomotion clip clock is scaled.
    void locomote(ActorState& actor,bool moving,float speed) {
        actor.locomotion_dirty|=moving!=actor.moving; actor.moving=moving;
        auto* controller=actor_controller(actor.id);
        if (!controller) return;
        controller->rate=1;
        if (!actor.alive || actor.node) return;
        const auto* clip=actor_clip(controller);
        if (!clip) return;
        const bool pc=is_pc(actor);
        // Movement interrupts an idle variant; other one-shot actions (swings, damage, emotes) finish first.
        const bool fidget=!clip->loops && (pc?clip->name=="Idle_LookAround":clip->state==3);
        if (!clip->loops && (!fidget || !moving)) return;
        const auto desired=pc?pc_locomotion(actor,moving):moving?npc_gait(actor,speed):state_clip(actor,3);
        if (desired==UINT32_MAX) return;
        // A rebind preserves the live run cursor; reconcile posture after its authored jump or a movement transition.
        // A wait clip of another stance (weapon drawn/stowed) is replaced at once; emote/gather loops are left alone.
        const bool running=moving && clip->loops && (pc?clip->name.starts_with("Run_"):clip->id==desired);
        const bool stance=pc && clip->name.starts_with("Wait_");
        // Swings rest in their class guard (authored jumptoid) until the next move; a guard of another class (a bare-hand
        // kick or taunt while armed) becomes this weapon class's guard.
        if (pc && !moving && !actor.locomotion_dirty && clip->name.starts_with("Guard_")) {
            if (const auto guard=pc_guard(actor); guard!=UINT32_MAX && guard!=clip->id) actor_action(actor,guard);
            return;
        }
        if (!running && (actor.locomotion_dirty || moving || stance || fidget) && clip->id!=desired) actor_action(actor,desired);
        actor.locomotion_dirty=false;
        if (moving && stride_speed(actor)>0) controller->rate=std::clamp(speed/stride_speed(actor),.25f,4.f);
        // NONCANONICAL idle variants (the server chose them): after 8-20 s resting on the wait clip a PC looks around
        // (Idle_LookAround authored-jumps back to Wait_BareHand, so unarmed only); NPCs/monsters play a one-shot wait-class clip.
        if (moving || clip->id!=desired) { actor.fidget_at={}; return; }
        const auto now=Clock::now();
        if (actor.fidget_at==Clock::time_point{}) { actor.fidget_at=now+std::chrono::milliseconds(8000+SDL_rand(12000)); return; }
        if (now<actor.fidget_at) return;
        actor.fidget_at={};
        const auto variant=pc?(actor.weapon.empty()?pc_clip(actor,"Idle_LookAround"):UINT32_MAX):
            pick_clip(actor,[](const ikv::ActorClip& c){return c.state==3 && !c.loops;},static_cast<unsigned>(SDL_rand(8)));
        if (variant!=UINT32_MAX) actor_action(actor,variant);
    }
    void movement_changed() {
        for (const auto& position:players_) move_actor(position,session_->tick);
    }
    // Original 004a6a80 skill-effect names by wire skill id (classindex*15+skillno); unlisted ids keep its "heal" default.
    static std::string_view skill_effect(int id) {
        static constexpr std::array<std::string_view,45> names{
            "depar","ofansif_dovus","sert_vurus","defansif_dovus","ofansif_dovus","dikkat_dagitma","durdurma","sakinma",
            "kanatma","savasnarasi","zihin_toplama","sarsilmaz","supurme","hedef_saptirma","kivilcim",
            "meteorit","konsantrasyon","heal","buz_oku","direncini_kirma","buz_bilgisi","ates_cemberi","heal","ates_bilgisi",
            "yildirim","buyu_bozma","heal","heal","kutup_ruzgari","zihinsaldirisi",
            "heal","zehirleme","heal","geri_dondurme","ruh_kalkani","heal","asit","panzehir","heal","fiziksel_direnc",
            "element_direnci","gazap","heal","heal","heal"};
        return id>=0 && id<45?names[static_cast<std::size_t>(id)]:std::string_view{};
    }
    // NONCANONICAL: the official server sent every cast clip (mink 004a4270 -> 0047bea0); skills.dat names none. VOD 14:52
    // shows the büyücü's area/self cast (ring at her feet) as unnamed PC clip 57, both arms flung up with a hop. Savaşçı
    // techniques (school 0) against enemies are the weapon class's strong strike (Double_* where authored, else its swing);
    // a spell at one enemy thrusts the free hand (51); a friendly single target raises the weapon before the face (53).
    std::uint32_t skill_clip(ActorState& actor,int school,std::string_view kind) {
        if (school==0 && (kind=="enemy" || kind=="enemies")) {
            const auto& w=actor.weapon;
            const auto strong=actor.pose==2?UINT32_MAX:w=="weaponranged"?pc_clip(actor,"Double_Shoot_FireArms"):
                w=="weaponlight"?pc_clip(actor,"Double_Hit_SharpWeapons"):UINT32_MAX;
            return strong!=UINT32_MAX?strong:pc_attack(actor);
        }
        return kind=="enemy"?51u:kind=="ally"?53u:57u;
    }
    std::optional<std::array<float,3>> entity_position(std::uint32_t id) const {
        const auto pose=std::find_if(poses_.begin(),poses_.end(),[&](const auto& p){return p.id==id;});
        if (pose==poses_.end()) return std::nullopt;
        return std::array<float,3>{pose->x,pose->ground_y,pose->z};
    }
    // Independent native gameplay selects proven source numeric clips, not an asserted official-server action policy.
    void action_event(const Json& event) {
        const auto type=event.value("type",std::string{});
        if (type!="combat" && type!="skill" && type!="gather") return;
        const auto source=event.at("source").get<std::uint32_t>();
        auto* actor=actor_state(source);
        const bool pc=actor && actor->source_class>=2 && actor->source_class<=7;
        // Attackers and casters turn toward their target (source models face local -Z, as in move_actor).
        if (const auto* other=actor_state(event.at("target").get<std::uint32_t>()); actor && other && other!=actor && actor->samples && other->samples) {
            const auto& from=actor->history[std::min<std::size_t>(actor->samples,4)-1];
            const auto& to=other->history[std::min<std::size_t>(other->samples,4)-1];
            if (from.x!=to.x || from.z!=to.z) actor->yaw=std::atan2(static_cast<float>(from.x-to.x),static_cast<float>(from.z-to.z));
        }
        if (type=="gather") { // Toplama: original PC clip 86 "Gathering" loops until the server ends or cancels it
            const auto phase=event.value("phase",std::string{});
            if (actor && actor->alive && pc) { if (phase=="start") { if (const auto clip=pc_clip(*actor,"Gathering"); clip!=UINT32_MAX) actor_action(*actor,clip,true); } else actor->locomotion_dirty=true; }
            // ID_MADEN_* / ID_CICEK cliplists: clip 1 maden_Wait (full), clip 3 frames 20-21 (picked) [INFERENCE: depleted pose].
            if (auto* node=actor_state(event.at("target").get<std::uint32_t>()); node && phase=="done") actor_action(*node,3);
            if (phase=="done" && session_ && source==session_->self) {
                std::string gained; for (const auto& item:event.value("items",Json::array())) if (item.is_string()) gained+=(gained.empty()?"":", ")+item_name(state_,item.get<std::string>());
                audio_->play_event("ui\\itemdrop.raw"); // NONCANONICAL: no gather sound is referenced by the client; the item-drop cue marks the gain
                notice("Toplandı: "+gained);
            }
            return;
        }
        if (type=="combat") {
            if (!actor || !actor->alive) return;
            // Auto-attack cycles the attacker's swing family: weapon class for PCs, `state` 5 clips for NPCs/monsters.
            // A "miss" swing plays the target's Dodge clip: PCs Dodge_BareHand (25) / Dodge_Armed (36), monsters `state` 6
            // (e.g. Goblin_Dodge) in animation.dat; the original server picked the clip, the pairing is NONCANONICAL.
            if (event.value("miss",false)) if (auto* target=actor_state(event.at("target").get<std::uint32_t>()); target && target->alive) {
                const auto dodge=is_pc(*target)?pc_clip(*target,"Dodge_",target->pose==2?"_BareHand":"_Armed"):state_clip(*target,6);
                if (dodge!=UINT32_MAX) actor_action(*target,dodge,true);
            }
            const auto clip=pc?pc_attack(*actor):state_clip(*actor,5,actor->swing++);
            if (clip!=UINT32_MAX) actor_action(*actor,clip,true);
            return;
        }
        const auto found=skill_catalog().find(event.at("skill").get<std::string>());
        if (session_ && source==session_->self) {
            const auto now=Clock::now();
            skill_global_ready_=now+std::chrono::seconds(1); // original 0044cfb0(1000)
            if (found!=skill_catalog().end()) skill_ready_[found.key()]=now+std::chrono::milliseconds(50*found->value("cooldown",0));
            cooldown_until_=std::max(skill_global_ready_,found!=skill_catalog().end()?skill_ready_[found.key()]:now);
            hud_dirty_=true;
        }
        // The event's skills.dat pair and target kind cover casters of every class (the catalog lists only the own class).
        const Json skill{{"source",event.at("skill_source")}};
        const int school=skill.at("source")[0],skillno=skill.at("source")[1].get<int>(),id=skillno<15?school*15+skillno:-1; // skillno 15 (Boz Ayı) has no wire id: 0*15+15 is Meteorit's
        const auto target=event.at("target").get<std::uint32_t>();
        if (actor && actor->alive && pc) { if (const auto clip=skill_clip(*actor,school,event.at("kind").get_ref<const std::string&>()); clip!=UINT32_MAX) actor_action(*actor,clip,true); }
        else if (actor && actor->alive) if (const auto clip=state_clip(*actor,5,actor->swing++); clip!=UINT32_MAX) actor_action(*actor,clip,true); // Şef casts: its attack clip (NONCANONICAL)
        if (effects_) effects_->launch(skill_effect(id),source,target); // thrown_psystem names travel source->target, others attach to target
        // Original 004a7958: even (female) source classes use femalesound when present.
        if (const auto* record=source_skill(skill)) {
            const auto& sound=actor && actor->source_class%2==0 && !record->female_sound.empty()?record->female_sound:record->sound;
            if (!sound.empty()) audio_->play_event(sound,entity_position(source));
        }
    }
    void chat_line(std::string text,Rgba color,int channel=chat_system,std::uint32_t from=0,std::string name={},Json link={}) {
        hud_dirty_=true;
        if (text.size()>600) { text.resize(600); while (!text.empty() && (static_cast<unsigned char>(text.back())&0xc0)==0x80) text.pop_back(); if (!text.empty() && static_cast<unsigned char>(text.back())>=0xc0) text.pop_back(); }
        if (log_.size()==64) log_.pop_front();
        log_.push_back({std::move(text),color,channel,from,std::move(name),std::move(link)});
    }
    // System notices go to the Mesajlaşma log (Sistem channel, original 0xffffff00) and flash briefly at the top of the screen.
    void notice(std::string message) {
        flash_=message; flash_color_=gold_rgba; flash_until_=Clock::now()+std::chrono::seconds(4);
        chat_line(std::move(message),original_rgba(chat_channels[chat_system].argb));
    }
    // Original 0048b660(0xffff7777, text): the red on-screen refusal; logged in the Sistem channel too.
    void alert(std::string message) { notice(std::move(message)); flash_color_=original_rgba(0xffff7777); }
    // 004b8230 "Dikkat" yes/no box (param 0): the action sent on ✓, its text, and ui\alert.raw (UI sound slot 0x77d2f4).
    void ask(Json action,std::string text) { confirm_=std::move(action); confirm_text_=std::move(text); audio_->play_event("ui\\alert.raw"); }
    void connect() {
        trade_.reset(); trade_seen_rows_=0; trade_seen_revision_=0; trade_pending_present_=false; trade_items_=Json::array(); trade_currency_=0;
        session_.reset(); count_=0; entities_.clear(); tick_epoch_.reset(); epoch_tick_=0;
        pending_mutations_.clear(); pending_bytes_=0; accepted_actors_.clear(); accepted_source_.reset();
        state_=Json::object(); state_pending_=false; players_.clear(); has_snapshot_=false;
        snapshot_source_.reset(); snapshot_revision_=painted_snapshot_revision_=0;
        try {
            session_=std::make_unique<ikv::ClientSession>(options_.host,options_.port,options_.ca,secret_.view(),options_.character);
            disconnected_=false; target_=0; notice("Güvenli bağlantı kuruldu; karakter doğrulanıyor.");
        } catch (const std::exception& e) {
            if (options_.ticks) throw;
            disconnected_=true; notice(std::string("Bağlantı kurulamadı: ")+e.what());
        }
    }
    void disconnect(const std::string& message) {
        // 'sjfr' 0045b6c5: "Bağlantınız kesildi çünkü %s" is the one-button 004b8230 box, which plays ui\warning.raw (0x77d314).
        disconnected_=true; loading_=false; session_.reset(); trade_.reset(); trade_seen_rows_=0; trade_pending_present_=false; notice(message);
        audio_->play_event("ui\\warning.raw");
    }
    void leave() { quit_=true; if (session_ && !disconnected_) session_->stop(); }
    void send(const Json& action) {
        if (!session_ || !session_->self || disconnected_) { notice("Dünyaya bağlı değilsiniz."); return; }
        try {
            const auto kind=action.value("action",std::string{});
            if (kind=="trade_accept") {
                if (action.contains("revision") && action.at("revision")!=trade_.revision()) {
                    notice("Gösterilen ticaret revizyonu ile onay uyuşmuyor."); return;
                }
                const auto accepted=trade_.accept_action();
                if (!accepted) { notice("Son sunucu teklifinin tüm şartlarını Alışveriş penceresinde görmeden onaylayamazsınız."); return; }
                session_->action(*accepted); hud_dirty_=true;
            } else {
                if (kind=="trade_offer") {
                    if (!trade_.active() || trade_.offer().value("phase",std::string{})!="open") {
                        notice("Önce ticaret daveti kabul edilmelidir."); return;
                    }
                    trade_.invalidate_consent();
                }
                session_->action(action);
            }
        } catch (const std::exception& e) { notice(std::string("İşlem gönderilemedi: ")+e.what()); }
    }
    Json target_action(const std::string& action) const { return Json{{"action",action},{"target",target_}}; }
    void state_changed() {
        entities_.clear();
        stat_floaters(state_); // before hud_values_ records the new values
        // The server's banker session (state "bank") opens the Banka window once; leaving range closes the session.
        if (state_.contains("bank")!=bank_session_) { bank_session_=!bank_session_; if (bank_session_) toggle_window(win_bank,true); }
        const auto& state=state_;
        const auto& cs=character_state(state_);
        // VOD 34:00 chat: "Yeniden canlanma için N para kaybettiniz." (server text; native takes the death_fee at death).
        if (cs.value("hp",1)<=0 && hud_values_.value("hp",1)>0 && hud_values_.contains("currency") && cs.contains("currency"))
            if (const auto lost=hud_values_.at("currency").get<std::int64_t>()-cs.at("currency").get<std::int64_t>(); lost>0)
                chat_line("Yeniden canlanma için "+std::to_string(lost)+" para kaybettiniz.",original_rgba(chat_channels[chat_system].argb));
        // VOD 34:00 yellow chat + red notice (server text): every death damages the worn equipment (world wear()).
        if (cs.value("hp",1)<=0 && hud_values_.value("hp",1)>0 && hud_values_.contains("inventory") && cs.contains("inventory") && hud_values_.at("inventory")!=cs.at("inventory")) {
            const std::string text="Öldüğünüz için eşyalarınız zarar gördü. Eşyalarınızı örs ile tamir edebilirsiniz.";
            chat_line(text,original_rgba(chat_channels[chat_system].argb)); notice(text);
        }
        if (state.contains("clock")) { // validated by ClientSession; original thgf consumer 004a4c90/004b6970
            const auto& clock=state.at("clock"); std::array<std::uint8_t,11> record{};
            for (std::size_t i=0;i<record.size();++i) record[i]=clock.at("environment")[i].get<std::uint8_t>();
            ikv::set_scene_clock(scene_,clock.at("hour").get<float>(),record);
            // thgf 004a4680 params 6/7: record fog start/end (+0xec/+0xf0), scaled by Görüş Mesafesi (0046b630/0046b660).
            if (const std::array<float,2> fog{clock.at("fog")[0].get<float>(),clock.at("fog")[1].get<float>()}; fog!=scene_.lighting.fog_record) {
                scene_.lighting.fog_record=fog; apply_view(scene_);
            }
            if (const int minute=static_cast<int>(scene_.lighting.source_hour*60); minute!=clock_minute_) { clock_minute_=minute; hud_dirty_=true; }
        }
        for (const auto* key:{"hp","max_hp","mana","max_mana","xp","level","currency","inventory","equipment","quests","buffs"}) {
            if (cs.contains(key) && (!hud_values_.contains(key) || hud_values_.at(key)!=cs.at(key))) {
                // Original 004a59d0: a level (stat 1) gain spawns "level_up" on that entity.
                if (std::string_view(key)=="level" && effects_ && session_ && hud_values_.contains(key) && hud_values_.at(key).is_number_integer() &&
                    cs.at(key).is_number_integer() && cs.at(key).get<std::int64_t>()>hud_values_.at(key).get<std::int64_t>()) {
                    effects_->trigger("level_up",session_->self);
                    // 004a5b78: the same stat handler writes 0x5987c8 to the chat log.
                    chat_line("Karakteriniz seviye atladı. Yeni yetenek ve özellik puanlarınızı dağıtabilirsiniz.",original_rgba(chat_channels[chat_system].argb));
                }
                hud_values_[key]=cs.at(key); hud_dirty_=true;
            }
        }
        for (const auto* key:{"catalogs","character","group","guild","xp_interval","zone","source_registry_id"}) {
            if (std::string_view(key)=="character") {
                if (state.contains(key) && state.at(key).contains("name") &&
                    (!hud_values_.contains("name") || hud_values_.at("name")!=state.at(key).at("name"))) {
                    hud_values_["name"]=state.at(key).at("name"); hud_dirty_=true;
                }
            } else if (state.contains(key) && (!hud_values_.contains(key) || hud_values_.at(key)!=state.at(key))) {
                hud_values_[key]=state.at(key); hud_dirty_=true;
            }
        }
        if (state.contains("entities")) {
            const auto& list=state.at("entities");
            bool changed=list.size()!=hud_entities_.size();
            if (!changed) for (std::size_t i=0;i<list.size();++i) {
                for (const auto* key:{"id","name","hp","max_hp","mana","max_mana","level","shop","quests","equipment","appearance","class_id"}) {
                    if (list[i].contains(key)!=hud_entities_[i].contains(key) ||
                        (list[i].contains(key) && list[i].at(key)!=hud_entities_[i].at(key))) changed=true;
                }
            }
            if (changed) {
                hud_entities_.clear();
                for (const auto& e:list) {
                    Json row=Json::object();
                    for (const auto* key:{"id","name","hp","max_hp","mana","max_mana","level","shop","quests","equipment","appearance","class_id"}) if (e.contains(key)) row[key]=e.at(key);
                    hud_entities_.push_back(std::move(row));
                }
                hud_dirty_=true;
            }
        }
        if (state.contains("entities")) for (const auto& e:state.at("entities")) {
            entities_.push_back({e.at("id").get<std::uint32_t>(),e.at("x").get<std::int32_t>(),e.at("y").get<std::int32_t>()});
        }
        entities_.push_back({session_->self,cs.at("x").get<std::int32_t>(),cs.at("y").get<std::int32_t>()});
        cache_original_ui_state();
        for (const auto& position:entities_) if (std::none_of(players_.begin(),players_.end(),[&](const auto& p){return p.id==position.id;})) move_actor(position,state.at("tick").get<std::uint32_t>());
    }
    void handle_events(const Json& events) {
        for (const auto& event:events) {
            const auto type=event.value("type",std::string{});
            action_event(event);
            // NONCANONICAL: original floaters are attribute deltas (0047c150) with no crit mark; a server "critical" hit floats
            // "Kritik!" over its target in the monster-damage yellow 0xffeeee00 beside the damage number.
            if (event.value("critical",false) && event.contains("target") && event.at("target").is_number_unsigned()) float_line(event.at("target").get<std::uint32_t>(),"Kritik!",0xffeeee00);
            // 0040b170: a channel-9 "Kritik!" line plays weapon\handgun\ateslisilah1.raw (UI sound slot 0x77d31c). Which crits the
            // server announced was server-only; NONCANONICAL: the native client plays it for its own critical hits.
            if (event.value("critical",false) && session_ && event.value("source",Json())==Json(session_->self)) audio_->play_event("weapon\\handgun\\ateslisilah1.raw");
            if (!trade_allowed_ && session_ && (type=="trade_invite" || (type=="trade" && event.value("phase",std::string{})=="invited" && event.value("initiator",0u)!=session_->self))) {
                if (type=="trade") send(Json{{"action","trade_cancel"}}); // 004a8afc: blocked requests are answered with 'Fqrt'
                continue;
            }
            if (type=="trade" || type=="trade_invite" || type=="trade_complete" || type=="trade_cancel" || type=="error") {
                const Json before=trade_.active()?trade_.offer():Json();
                if (trade_.receive(event,session_->self)) {
                    hud_dirty_=true;
                    const auto phase=trade_.offer().value("phase",std::string{});
                    if (trade_.revision()!=trade_seen_revision_ || phase!=trade_seen_phase_ || !trade_.active()) {
                        trade_seen_revision_=trade_.revision(); trade_seen_phase_=phase;
                        trade_seen_rows_=0; trade_pending_present_=false; scroll_[win_trade]=0;
                    }
                    if (trade_.active()) toggle_window(win_trade,true);
                    if (type=="trade" && trade_.active()) { // 004a8940 trade states
                        const auto& now=trade_.offer();
                        const bool was_open=before.is_object() && before.at("phase")=="open";
                        const auto accepted=[](const Json& t){return t.at("own").at("accepted")==true || t.at("other").at("accepted")==true;};
                        if (phase=="open" && !was_open) { trade_items_=Json::array(); trade_currency_=0; notice("Alışveriş başlatılmıştır."); } // state 1, 0x598f30
                        else if (was_open && accepted(before) && !accepted(now)) notice("Alışveriş durumu yeniden konumlandırılmıştır."); // state 5, 0x598e80
                    }
                }
                // A refused offer leaves the authoritative own terms in force: edit from those again.
                if (const auto code=event.value("code",std::string{}); type=="error" && trade_.active() && trade_.offer().at("phase")=="open" &&
                    (code=="currency" || code=="quantity" || code=="equipped" || code=="bound" || code=="trade" || code=="request")) {
                    const auto& own=trade_.offer().at("own");
                    trade_items_=Json::array();
                    for (const auto& item:own.at("items")) trade_items_.push_back({{"uid",item.at("uid")},{"quantity",item.at("quantity")}});
                    trade_currency_=own.at("currency").get<std::uint64_t>();
                }
            }
            if (sound_map_.contains(type) && sound_map_.at(type).is_string()) {
                // Explicit NONCANONICAL operator mapping: path relative to the original base, under audio/.
                const auto relative=ikv::original_audio_path(sound_map_.at(type).get<std::string>());
                if (relative.starts_with("audio/")) audio_->play_event(std::string_view(relative).substr(6));
                else notice("Ses eşlemesi özgün audio/ klasöründe olmalı: "+type);
            }
            if (type=="chat") {
                const auto wire=event.value("channel",std::string{});
                int channel=chat_say;
                for (int i=0;i<chat_system;++i) if (wire==chat_channels[static_cast<std::size_t>(i)].wire) channel=i;
                const auto name=event.value("name",std::string{"Sohbet"});
                if (channel==chat_whisper && session_ && event.value("from",0u)!=session_->self) { whisper_from_=event.value("from",0u); whisper_from_name_=name; }
                const auto from=event.value("from",0u);
                std::string text=event.value("text",std::string{}); Json link;
                if (const auto l=event.find("link");l!=event.end() && l->is_object() && l->contains("item") && l->at("item").is_string() && l->contains("definition") && l->at("definition").is_object() &&
                    (!l->contains("prefixes") || l->at("prefixes").is_array())) { // 0040b170: "[<prefixes> <name>]"
                    Json entry{{"item",l->at("item")}}; if (l->contains("prefixes")) entry["prefixes"]=l->at("prefixes");
                    link={{"entry",entry},{"state",{{"catalogs",{{"items",{{l->at("item").get<std::string>(),l->at("definition")}}}}}}}};
                    text="["+entry_name(link.at("state"),entry)+"]";
                }
                chat_line("["+name+"]"+text,original_rgba(chat_channels[static_cast<std::size_t>(channel)].argb),channel,
                          session_ && from!=session_->self?from:0,name,std::move(link));
            }
            else if (type=="error") alert(event.value("message",event.value("code",std::string{"İşlem reddedildi"}))); // refusals in the original red (0048b660)
            else if (type=="ack") { // server-confirmed item moves 'mkcy' 004a9690 / 'skua' 004a9880: ui\eat.raw when a worn slot is involved, else ui\itemdrop.raw
                const auto action=event.value("action",std::string{});
                if (action=="equip" || action=="unequip") audio_->play_event("ui\\eat.raw");
                else if (action=="item_move" || action=="bank_deposit" || action=="bank_withdraw") audio_->play_event("ui\\itemdrop.raw");
            }
            else if (type=="arena") chat_line(event.value("text",std::string{}),original_rgba(chat_channels[chat_system].argb)); // Er Meydanı server lines (world.cpp activities)
            else if (type=="notice") notice(event.value("text",std::string{})); // server line shown centred and in chat (VOD 49:45 free reset)
            else if (type=="dialogue") {
                // Merchants open Satıcı; questgivers and the banker open the Konuş window (the bank itself opens from İşlemler → Kasamı göster).
                // A merchant with quest pages here opens both (classes.dat merchant+questGiver NPCs, e.g. Agop).
                if (event.contains("target") && event.at("target").is_number_unsigned()) target_=event.at("target").get<std::uint32_t>();
                const bool shop=!event.value("shop",Json::array()).empty();
                const auto pages=dialogue_quests(event.value("quests",Json::array()),event.value("target",0u),character_state(state_));
                if (shop) { notice(event.value("text",std::string{})); toggle_window(win_vendor,true); }
                if (!shop || !pages.empty()) { dialogue_=event; dialogue_page_=pages.empty()?-1:1; } // -1: first quest page once painted
                // 00419683: talking to Agah Efendi below level 2 opens Seviye1c (İlk Görev, Nasıl Yürünür) over Seviye1b.
                if (const Json* e=entity(target_); e && e->value("name",std::string{})=="Agah Efendi" && character_state(state_).value("level",2)<2) {
                    tutorial_target_=target_; open_help("Seviye1c");
                }
            }
            else if (type=="quest") { // server quest stages as the VOD's yellow system chat lines (world.cpp quest block)
                const auto title=event.value("name",std::string{})+" ["+std::to_string(event.value("level",1))+"]";
                const auto stage=event.value("stage",std::string{});
                const auto yellow=original_rgba(chat_channels[chat_system].argb);
                if (stage=="accepted") chat_line("Yeni bir görev aldınız.",yellow);
                else if (stage=="progress") {
                    const auto line=title+" : ("+std::to_string(event.value("progress",0))+"/"+std::to_string(event.value("count",0))+")";
                    chat_line(line,yellow);
                    if (session_) float_line(session_->self,line,0xffff0000); // VOD 24:30: the same line floats red over the player
                }
                else if (stage=="ready") chat_line(title+" görevini teslim edebilirsiniz!",yellow);
                else if (stage=="completed") { chat_line(title+" görevini bitirdiniz!",yellow); if (event.contains("text")) chat_line(event.value("text",std::string{}),yellow); }
                else if (stage=="expired") chat_line(title+" görevinin süresi doldu!",yellow);
            }
            else if (type=="death") {
                if (event.at("entity")==session_->self) { resurrect_at_=Clock::now()+std::chrono::milliseconds(15000); resurrect_shown_=-2; hud_dirty_=true; }
                else if (const Json* dead=entity(event.at("entity").get<std::uint32_t>()); dead && dead->contains("name")) notice("Dünyada ölüm: "+dead->at("name").get<std::string>()); // an entity outside this frame has no name to show
            }
            else if (type=="repair") { // helps.xml Para: örs repair; original ui\repair.raw (0x597c9c). Chat wording NONCANONICAL.
                audio_->play_event("ui\\repair.raw");
                chat_line("Eşyalarınız tamir edildi ("+std::to_string(event.value("cost",0))+" Lira).",original_rgba(chat_channels[chat_system].argb));
            }
            else if (type=="reset_offer") { // GBM-0 / GBM 0-B: Dikkat confirm of the server price (wording NONCANONICAL), then point_reset
                const bool crafts=event.value("reset",std::string{})=="crafts";
                ask(Json{{"action","point_reset"},{"target",event.value("target",0u)},{"price",event.value("price",std::int64_t{0})}},
                    std::string(crafts?"Üretim ve toplama":"Yetenek ve direnç")+" puanlarınız "+std::to_string(event.value("price",std::int64_t{0}))+" Lira karşılığında sıfırlanacak. Emin misiniz?");
            }
            else if (type=="point_reset") chat_line(std::string(event.value("reset",std::string{})=="crafts"?"Üretim ve toplama":"Yetenek ve direnç")+" puanlarınız sıfırlandı ("+std::to_string(event.value("price",std::int64_t{0}))+" Lira).",original_rgba(chat_channels[chat_system].argb));
            else if (type=="loot_open") { // 'sdbt' answer to a Ganimet click: 3x3 window (00436140), shown while a slot is left
                const auto& items=event.value("items",Json::array());
                loot_=std::any_of(items.begin(),items.end(),[](const Json& slot){return slot.is_object();})?event:Json(nullptr);
            }
            else if (type=="loot" && event.contains("item")) { // VOD 34:00 "[Unalli çantadan aldı][Savunma Artırıcı Kanvaslı Deri Eldiven]"
                Json taken{{"item",event.at("item")}}; if (event.contains("prefixes")) taken["prefixes"]=event.at("prefixes");
                const auto who=state_.contains("character")?state_.at("character").value("name",std::string{}):std::string{};
                chat_line("["+who+" çantadan aldı]["+entry_name(state_,taken)+"]",{230,60,60,255});
                // 'fatl' 0043f560 empties the taken slot; the window stays until the box itself goes away.
                if (loot_.is_object() && loot_.value("target",0u)==event.value("target",0u) && event.value("slot",9u)<loot_.at("items").size())
                    loot_.at("items")[event.at("slot").get<std::size_t>()]=nullptr;
            }
            else if (type=="craft") { // original ui\craft.raw (UI sound bank slot +0xac, loader 0048eca0)
                audio_->play_event("ui\\craft.raw");
                notice(item_name(state_,event.value("item",std::string{}))+" üretildi.");
            }
            else if (type=="trade_invite") notice("Ticaret daveti: Alışveriş penceresinin onay düğmesiyle kabul edin."); // NONCANONICAL: the original opens at once
            // States 3/6 of 004a8940 hide the window.
            else if (type=="trade_complete" || type=="trade_cancel") {
                notice(type=="trade_complete"?"Alışveriş başarı ile sonlandırılmıştır.":"Alışveriş iptal edilmiştir."); // 0x598ee0 / 0x598e08
                trade_items_.clear(); trade_currency_=0;
                if (is_open(win_trade)) toggle_window(win_trade);
            }
            else if (type=="group") {
                const auto members=event.value("members",Json::array());
                group_members_=members.is_array()?members:Json::array(); group_leader_=event.value("leader",0u);
                notice("Grup durumu güncellendi.");
            }
            // 004a7f40 / 004a8010: "Dikkat" yes/no prompts (accept callbacks 004102b0/00410290) only while G allows invites.
            else if (type=="group_invite" && invites_allowed_) {
                ask(Json{{"action","group_accept"}},event.value("name",std::string{"?"})+" sizi grubuna davet ediyor. Kabul ediyor musunuz?");
            }
            else if (type=="arena_stake") { // /kpsavas: Dikkat confirm (server text, NONCANONICAL), then arena_join with the same code and stake
                ask(Json{{"action","arena_join"},{"code",event.value("code",0)},{"stake",event.value("stake",0)}},event.value("text",std::string{}));
            }
            else if (type=="guild_invite" && invites_allowed_) { // the original %s is server text; this wording is NONCANONICAL
                const auto& guild=event.value("guild",Json::object());
                ask(Json{{"action","guild_accept"}},event.value("name",std::string{"?"})+" sizi "+(guild.is_object()?guild.value("name",std::string{}):std::string{})+" loncasına davet ediyor. Kabul ediyor musunuz?");
            }
            else if (type=="complaint") notice("Şikayet: "+event.value("name",std::string{})+" → "+event.value("target_name",std::string{})); // administrators only
            else if (type=="announce") notice("Duyuru: "+event.value("text",std::string{})); // NONCANONICAL web-admin broadcast
            else if (type=="emote") { // Rol yapma gesture: RP_* clip from data/animation.dat; clip 0 ("/kalk", original 'toah') stands up
                if (auto* actor=actor_state(event.value("entity",0u)); actor && actor->alive) {
                    const unsigned clip=event.value("clip",0u);
                    actor_action(*actor,clip?clip:pc_locomotion(*actor,false),true);
                }
            }
        }
    }
    void submit_input() {
        if (input_.empty()) return;
        try {
            if (input_.starts_with("/{")) { // developer typed action
                const auto action=Json::parse(input_.substr(1));
                if (!action.is_object() || !action.contains("action")) throw std::invalid_argument("typed action object required");
                send(action);
            } else {
                const auto& channel=chat_channels[static_cast<std::size_t>(channel_)];
                Json action{{"action","chat"},{"channel",channel.wire},{"text",input_}};
                if (channel_==chat_whisper) {
                    if (!whisper_to_) { notice("Fısıldama için '/f isim ' yazın veya hedefte Özel mesaj seçin."); return; }
                    action["to"]=whisper_to_;
                }
                chat_hidden_[static_cast<std::size_t>(channel_)]=false; // help: sending re-enables a filtered channel
                send(action);
                if (channel.once) channel_=chat_previous_;
            }
            input_.clear();
        } catch (...) { notice("İşlem biçimi: /{\"action\":\"...\",...}; sohbet için düz metin."); }
    }
    void input(float dt) {
        SDL_Event event{};
        while (SDL_PollEvent(&event)) {
            if (event.type!=SDL_EVENT_MOUSE_MOTION) hud_dirty_=true;
            else { // cWindow hover (00491887): the tooltip belongs to the window under the pointer and follows it (0048de14)
                const auto [mx,my]=pixel(event.motion.x,event.motion.y);
                int top=static_cast<int>(hits_.size())-1;
                while (top>=0 && !inside({hits_[static_cast<std::size_t>(top)].x,hits_[static_cast<std::size_t>(top)].y,hits_[static_cast<std::size_t>(top)].w,hits_[static_cast<std::size_t>(top)].h},mx,my)) --top;
                if (top!=hover_hit_ || !tip_.empty()) { hover_hit_=top; hud_dirty_=true; }
            }
            // helps.xml 'Kamera Yönetimi': hold the right button and move the mouse to turn the view (W/A/S/D shortcut).
            if (event.type==SDL_EVENT_MOUSE_MOTION && (event.motion.state&SDL_BUTTON_RMASK) && renderer_) {
                auto camera=renderer_->camera();
                camera.yaw+=event.motion.xrel*.01f; camera.pitch=std::clamp(camera.pitch+event.motion.yrel*.01f,.08f,1.45f);
                renderer_->set_camera(camera);
            }
            if (event.type==SDL_EVENT_QUIT || event.type==SDL_EVENT_WINDOW_CLOSE_REQUESTED) leave();
            if (event.type==SDL_EVENT_WINDOW_FOCUS_LOST) { editing_=false; drag_=nullptr; dragging_=false; }
            if (event.type==SDL_EVENT_MOUSE_MOTION && !drag_.is_null() && (event.motion.state&SDL_BUTTON_LMASK)) {
                std::tie(drag_x_,drag_y_)=pixel(event.motion.x,event.motion.y);
                if (!dragging_ && std::hypot(drag_x_-press_x_,drag_y_-press_y_)>static_cast<float>(S(4))) dragging_=true;
                if (dragging_) hud_dirty_=true;
            }
            if (event.type==SDL_EVENT_MOUSE_BUTTON_UP && event.button.button==SDL_BUTTON_LEFT && !drag_.is_null()) {
                const auto [x,y]=pixel(event.button.x,event.button.y);
                if (dragging_) drop(x,y); else hud_click(press_x_,press_y_); // a press without movement is the element's click
                drag_=nullptr; dragging_=false;
            }
            if (event.type==SDL_EVENT_TEXT_INPUT && editing_ && input_.size()+std::char_traits<char>::length(event.text.text)<=1024) { input_+=event.text.text; chat_command(); }
            // 0041c2a0 -> 00468010: releasing PrintScreen (DIK_SYSRQ) saves EKRAN_<_strdate>_<_strtime> with '/' -> '-' and
            // ':' -> '.' into the working directory: BMP with Shift, else TGA with Ctrl, else JPG.
            if (event.type==SDL_EVENT_KEY_UP && event.key.key==SDLK_PRINTSCREEN && session_) {
                const auto now=std::time(nullptr); std::tm local{}; localtime_r(&now,&local);
                char name[40]; std::strftime(name,sizeof name,"EKRAN_%m-%d-%y_%H.%M.%S",&local);
                screenshot_request_=std::string(name)+((event.key.mod&SDL_KMOD_SHIFT)?".BMP":(event.key.mod&SDL_KMOD_CTRL)?".TGA":".JPG");
            }
            if (event.type==SDL_EVENT_KEY_DOWN && !event.key.repeat) {
                const auto key=event.key.key;
                // helps.xml: Ctrl+C answers the last whisper received, Ctrl+X whispers the last recipient again.
                const bool splitting=confirm_.is_object() && confirm_.value("action",std::string{})=="split";
                if (splitting && ((key>=SDLK_0 && key<=SDLK_9) || (key>=SDLK_KP_1 && key<=SDLK_KP_0) || key==SDLK_BACKSPACE || key==SDLK_RETURN || key==SDLK_KP_ENTER || key==SDLK_ESCAPE)) {
                    if (key==SDLK_RETURN || key==SDLK_KP_ENTER) handle("confirm-yes"); // 004336c0 accepts on Enter
                    else if (key==SDLK_ESCAPE) handle("confirm-no");
                    else if (key==SDLK_BACKSPACE) { if (!split_amount_.empty()) split_amount_.pop_back(); }
                    else if (split_amount_.size()<4) split_amount_+=static_cast<char>('0'+(key>=SDLK_0 && key<=SDLK_9?key-SDLK_0:(key-SDLK_KP_1+1)%10));
                } else if ((event.key.mod&SDL_KMOD_CTRL) && (key==SDLK_C || key==SDLK_X)) {
                    if (key==SDLK_C) whisper(whisper_from_,whisper_from_name_); else whisper(whisper_to_,whisper_name_);
                    if (channel_==chat_whisper) editing_=true;
                } else if (key==SDLK_ESCAPE) {
                    if (editing_) editing_=false;
                    else if (!confirm_.is_null()) confirm_=nullptr;
                    else if (context_) context_=0;
                    else if (!windows_.empty()) windows_.pop_back();
                    else system_menu_=!system_menu_; // original Esc menu "Ana Menü": Oyuna Dön / Ayarlar / Çıkış
                } else if (key==SDLK_RETURN || key==SDLK_KP_ENTER) {
                    if (editing_) { submit_input(); editing_=false; }
                    else editing_=true;
                } else if (editing_) {
                    if (key==SDLK_BACKSPACE && !input_.empty()) pop_utf8(input_);
                } else if (key>=SDLK_1 && key<=SDLK_9) use_slot(static_cast<std::size_t>(key-SDLK_1));
                else if (key==SDLK_0) use_slot(9);
                // Original bar tooltips 0x59b540..0x59b6a4: (K)Karakter, (C)Çanta, (Y)Yetenek, (G)Görev, (H)Harita, F1 Yardım,
                // F4 camera lock. (B)Bağlantılar is the Social window; P (group) is NONCANONICAL.
                else if (key==SDLK_K) toggle_window(win_character);
                else if (key==SDLK_C) toggle_window(win_bag);
                else if (key==SDLK_Y) toggle_window(win_skills);
                else if (key==SDLK_U) toggle_window(win_craft);
                else if (key==SDLK_G) toggle_window(win_quests);
                else if (key==SDLK_H) map_open_=!map_open_;
                else if (key==SDLK_P) toggle_window(win_group);
                else if (key==SDLK_B) toggle_window(win_contacts); // (B)Bağlantılar Penceresi 0x58e28c
                else if (key==SDLK_Z) { // 004b73ab: Z sits ("/otur", RP_Cok clip 82) or, already sitting, stands ('toah')
                    const auto* controller=session_?actor_controller(session_->self):nullptr;
                    const auto* clip=controller?actor_clip(controller):nullptr;
                    send(Json{{"action","chat"},{"channel","roleplay"},{"text",clip && clip->id==82?"/kalk":"/otur"}});
                }
                else if (key==SDLK_F1) { help_book_.clear(); toggle_window(win_help); }
                else if (key==SDLK_F4) camera_locked_=!camera_locked_;
                else if (key==SDLK_TAB && !labels_.empty()) {
                    const auto current=std::find_if(labels_.begin(),labels_.end(),[&](const auto& l){return l.id==target_;});
                    target_=current==labels_.end() || std::next(current)==labels_.end()?labels_.front().id:std::next(current)->id;
                }
                else if (key==SDLK_R && disconnected_) reconnect_=true;
            }
            if (event.type==SDL_EVENT_MOUSE_WHEEL) {
                const auto [x,y]=pixel(event.wheel.mouse_x,event.wheel.mouse_y);
                const int steps=static_cast<int>(event.wheel.y);
                const auto window=std::find_if(window_rects_.rbegin(),window_rects_.rend(),[&](const auto& w){return inside(w.second,x,y);});
                if (inside(map_rect_,x,y)) map_zoom_=std::clamp(steps>0?map_zoom_/2:steps<0?map_zoom_*2:map_zoom_,1,4); // 512..2048 texels
                else if (window!=window_rects_.rend()) { auto& scroll=scroll_[static_cast<std::size_t>(window->first)]; scroll=std::max(0,scroll-steps); }
                else if (inside(chat_rect_,x,y)) chat_scroll_=std::max(0,chat_scroll_+steps);
                else if (!editing_) options_.camera.distance=std::clamp(options_.camera.distance-event.wheel.y*2.f,6.f,60.f);
            }
            if (event.type==SDL_EVENT_MOUSE_BUTTON_DOWN && (event.button.button==SDL_BUTTON_LEFT || event.button.button==SDL_BUTTON_RIGHT) && !quit_) {
                const auto [x,y]=pixel(event.button.x,event.button.y);
                const bool right=event.button.button==SDL_BUTTON_RIGHT;
                if (!right) chat_link_open_=nullptr;
                if (const auto link=std::find_if(chat_link_hits_.begin(),chat_link_hits_.end(),[&](const auto& h){return inside(h.first,x,y);}); right && link!=chat_link_hits_.end() && link->second<log_.size()) {
                    chat_link_open_=log_[link->second].link; chat_link_x_=static_cast<int>(x); chat_link_y_=static_cast<int>(y); hud_dirty_=true;
                } else if (!right) {
                    // A drag source (skill icon, bag item, quickbar slot) that is the topmost element waits for the release.
                    const auto hit=std::find_if(hits_.rbegin(),hits_.rend(),[&](const Hit& h){return inside({h.x,h.y,h.w,h.h},x,y);});
                    const auto source=std::find_if(drags_.rbegin(),drags_.rend(),[&](const auto& d){return inside(d.first,x,y);});
                    if (session_ && hit!=hits_.rend() && source!=drags_.rend() && source->first==std::array<int,4>{hit->x,hit->y,hit->w,hit->h}) {
                        drag_=source->second; press_x_=drag_x_=x; press_y_=drag_y_=y; dragging_=false;
                    } else if (!hud_click(x,y) && session_ && session_->self) {
                        context_=0;
                        editing_=false;
                        pick(x,y);
                    }
                } else if (const auto hit=std::find_if(hits_.rbegin(),hits_.rend(),[&](const Hit& h){return inside({h.x,h.y,h.w,h.h},x,y);}); hit!=hits_.rend()) {
                    if (hit->value.starts_with("target:")) { handle(hit->value); context_=1; }
                    else if (hit->value.starts_with("slot:")) use_slot(std::stoul(hit->value.substr(5))); // helps.xml: right click uses a slot
                    else if (hit->value=="context") context_=1;
                } else if (session_ && session_->self) {
                    context_=0;
                    editing_=false;
                    if (const auto id=label_at(x,y)) { target_=id; context_=1; }
                }
            }
        }
        if (!editing_ && (SDL_GetWindowFlags(window_)&SDL_WINDOW_INPUT_FOCUS)) {
            const bool* keys=SDL_GetKeyboardState(nullptr);
            auto camera=renderer_->camera();
            camera.yaw+=dt*1.5f*(static_cast<float>(keys[SDL_SCANCODE_D]||keys[SDL_SCANCODE_RIGHT])-static_cast<float>(keys[SDL_SCANCODE_A]||keys[SDL_SCANCODE_LEFT]));
            camera.pitch=std::clamp(camera.pitch+dt*(static_cast<float>(keys[SDL_SCANCODE_W]||keys[SDL_SCANCODE_UP])-static_cast<float>(keys[SDL_SCANCODE_S]||keys[SDL_SCANCODE_DOWN])),.08f,1.45f);
            renderer_->set_camera(camera);
        }
    }
    void handle(const std::string& value) {
        if (value=="reconnect") reconnect_=true;
        else if (value=="quit") leave();
        else if (value=="channel") select_channel(channel_==chat_say?2:channel_==2?1:chat_say); // persistent Söyle/Grup/Klan
        else if (value=="chat-filter") chat_filter_open_=!chat_filter_open_;
        else if (value.starts_with("filter:")) { const auto i=static_cast<std::size_t>(value[7]-'0'); if (i<chat_hidden_.size()) chat_hidden_[i]=!chat_hidden_[i]; }
        else if (value=="map") map_open_=false;
        else if (value.starts_with("help-book:")) open_help(value.substr(10));
        else if (value=="input") editing_=true;
        else if (value=="guild-create") {
            if (input_.empty() || input_.size()>64) notice("Sohbet satırına geçerli lonca adı yazıp Lonca kur düğmesini kullanın.");
            else { send(Json{{"action","guild_create"},{"name",input_}}); input_.clear(); }
        }
        else if (value=="guild-donate") { // NONCANONICAL guild_donate: the Klan Parası amount typed on the chat line
            char* end=nullptr; const long long amount=input_.empty()?0:std::strtoll(input_.c_str(),&end,10);
            if (amount<=0 || *end) notice("Sohbet satırına bağış miktarını (Klan Parası) yazıp Bağış yap düğmesini kullanın.");
            else { send(Json{{"action","guild_donate"},{"amount",amount}}); input_.clear(); }
        }
        else if (value=="volume+") { options_.volume=std::min(1.f,options_.volume+.1f); audio_->mixer.volume(options_.volume); }
        else if (value=="volume-") { options_.volume=std::max(0.f,options_.volume-.1f); audio_->mixer.volume(options_.volume); }
        else if (value=="music+") { options_.music_volume=std::min(1.f,options_.music_volume+.1f); audio_->music_volume(options_.music_volume); }
        else if (value=="music-") { options_.music_volume=std::max(0.f,options_.music_volume-.1f); audio_->music_volume(options_.music_volume); }
        else if (value=="fullscreen") { options_.fullscreen=!options_.fullscreen; if (!SDL_SetWindowFullscreen(window_,options_.fullscreen)) notice("Tam ekran geçişi başarısız."); }
        // 004372c0 '+' / 00437450 '−' send 'mmrt' at once: Ctrl ±500000/100000, Alt 50000/10000, Shift 5000/1000, else 500/100 (DIK table
        // 0x5d7e94: Ctrl 0x5d7eb1/0x5d7f31, Alt 0x5d7ecc/0x5d7f4c, Shift 0x5d7ebe/0x5d7eca); '+' stays below 0xbebc201 and the money carried.
        else if (value=="currency+" || value=="currency-") {
            const auto mod=SDL_GetModState(); const bool up=value.back()=='+';
            const std::uint64_t step=(mod&SDL_KMOD_CTRL)?(up?500000:100000):(mod&SDL_KMOD_ALT)?(up?50000:10000):(mod&SDL_KMOD_SHIFT)?(up?5000:1000):(up?500:100);
            if (!trade_.active() || trade_.offer().at("phase")!="open") notice("Önce ticaret daveti kabul edilmelidir.");
            else if (!up) { if (trade_currency_) { trade_currency_-=std::min(step,trade_currency_); send_offer(); } }
            else if (trade_currency_+step>200000000) notice("Bu kadar büyük para transferi yapamazsınız."); // 0x592080
            else if (static_cast<std::int64_t>(trade_currency_+step)>character_state(state_).value("currency",std::int64_t{0})) notice("Yeterli paranız yok! Daha fazla artıramazsınız."); // 0x592050
            else { trade_currency_+=step; send_offer(); }
        }
        // 0042e9e0 ✓: 'qert' carries a checksum of the peer's shown terms, here the revision ClientTrade saw presented.
        else if (value=="trade-accept" && trade_.active() && session_) {
            const auto& t=trade_.offer();
            if (t.at("phase")=="invited" && t.at("initiator")==session_->self) notice("Karşı tarafın davet onayı bekleniyor.");
            else if (t.at("own").at("accepted")!=true) send(Json{{"action","trade_accept"},{"revision",trade_.revision()}});
        }
        else if (value=="offer-item" && !selected_uid_.empty()) {
            if (std::any_of(trade_items_.begin(),trade_items_.end(),[&](const auto& i){return i.at("uid")==selected_uid_;})) withdraw_item(selected_uid_);
            else offer_item(selected_uid_);
        }
        else if (value=="item-use" && !selected_uid_.empty()) use_item(selected_uid_);
        else if (value=="item-sell" && !selected_uid_.empty()) sell_item(selected_uid_,false);
        else if (value=="item-split" && !selected_uid_.empty()) open_split(selected_uid_);
        else if (value=="item-equip" && !selected_uid_.empty()) {
            if (dead()) notice("Ölüyken bunu yapamazsınız!");
            else if (const Json* entry=inventory_item(character_state(state_),selected_uid_); entry && !wearable(*entry)) alert("Bu eşyayı giyemezsiniz.");
            else send(Json{{"action","equip"},{"uid",selected_uid_}});
        } else if (value.starts_with("target:")) {
            const auto id=character_number(value.substr(7));
            if (id>UINT32_MAX) throw std::invalid_argument("target identifier exceeds uint32");
            target_=static_cast<std::uint32_t>(id);
        }
        else if (value.starts_with("uid:")) { // 0042f690: Shift splits, else Ctrl links the item in chat
            selected_uid_=value.substr(4);
            if (SDL_GetModState()&SDL_KMOD_SHIFT) open_split(selected_uid_); else if (SDL_GetModState()&SDL_KMOD_CTRL) link_item(selected_uid_);
        }
        else if (value.starts_with("skill-pick:")) { // 0044cf50: only an unlocked (row level) icon becomes the selection
            const int i=std::stoi(value.substr(11)); if (character_state(state_).value("level",0)>=10*(i/3)) skill_slot_=i;
        }
        else if (value=="skill-plus" && skill_slot_>=0) { // 0044d2a0: needs Kalan > 0, the row level and rank+pending < 15
            const auto& st=character_state(state_); const auto i=static_cast<std::size_t>(skill_slot_);
            const int pending=std::accumulate(skill_pending_.begin(),skill_pending_.end(),0);
            if (state_.value("skill_points",0)-pending<=0) {}
            else if (st.value("level",0)<10*(skill_slot_/3)) notice("Bu yetenek için seviyeniz yetersiz!"); // 0x593140
            else if (skill_slots(st)[i].rank+skill_pending_[i]>=15) notice("Bir yetenekteki toplam puan 15'i geçemez!"); // 0x593114
            else ++skill_pending_[i];
        }
        else if (value=="skill-minus" && skill_slot_>=0 && skill_pending_[static_cast<std::size_t>(skill_slot_)]>0) --skill_pending_[static_cast<std::size_t>(skill_slot_)]; // 0044d480
        else if (value.starts_with("stat-plus:")) ++stat_pending_[value.substr(10)]; // offered only while that pool has points left
        else if (value.starts_with("stat-minus:")) { const auto f=stat_pending_.find(value.substr(11)); if (f!=stat_pending_.end() && !--f->second) stat_pending_.erase(f); }
        else if (value=="stat-ok" && !stat_pending_.empty()) { // 00402a80: each group's new totals ('targ'/'cnch'), then the pending points clear
            Json points=Json::object(); for (const auto& [stat,count]:stat_pending_) points[stat]=count;
            send(Json{{"action","stat_points"},{"points",std::move(points)}}); stat_pending_.clear();
        }
        else if (value=="skill-ok") { // 0044d4e0: nothing pending closes the window, else every pending point goes at once
            const auto slots=skill_slots(character_state(state_)); Json points=Json::object();
            for (std::size_t i=0;i<slots.size();++i) if (skill_pending_[i] && !slots[i].id.empty()) points[slots[i].id]=skill_pending_[i];
            if (points.empty()) { toggle_window(win_skills); skill_slot_=-1; }
            else send(Json{{"action","skill_learn"},{"skills",std::move(points)}});
            skill_pending_.fill(0); // the ABILITY answer resets the window (004a4030 → 0044ce90); state carries the new ranks
        }
        else if (value=="skill-close") { toggle_window(win_skills); skill_slot_=-1; skill_pending_.fill(0); } // 0044d6e0 → 0044d3d0
        else if ((value=="help+" || value=="help-") && !scene_.help.empty()) {
            const auto [first,last]=help_range();
            help_page_=first+(help_page_-first+(value=="help+"?1:last-first-1))%(last-first); scroll_[win_help]=0;
        }
        else if (value=="help-image+" && !help_images_.empty()) help_image_page_=(help_image_page_+1)%static_cast<int>(help_images_.size());
        else if (value=="help-image-" && !help_images_.empty()) help_image_page_=(help_image_page_+static_cast<int>(help_images_.size())-1)%static_cast<int>(help_images_.size());
        else if (value=="context") context_=context_?0:1;
        else if (value.starts_with("contacts:")) { contacts_tab_=value.back()=='1'; scroll_[win_contacts]=0; }
        else if (value=="kabadayi") ask(Json{{"action","kabadayi"}},"Kabadayı moduna geçmek istediğinize emin misiniz?");
        else if (value=="clan-leave") ask(target_action("clan_leave"),"Klandan ayrılmak size Şan ve Klan parası kaybettirecektir. Emin misiniz?");
        else if (value=="confirm-yes") {
            if (confirm_=="quit") leave();
            else if (confirm_.is_object() && confirm_.value("action",std::string{})=="split") { // 004336c0: 0 < n <= count, else nothing is sent
                int n=0; const auto [end,error]=std::from_chars(split_amount_.data(),split_amount_.data()+split_amount_.size(),n);
                const Json* entry=inventory_item(character_state(state_),confirm_.value("uid",std::string{}));
                if (error==std::errc{} && end==split_amount_.data()+split_amount_.size() && entry && n>0 && n<=entry->value("quantity",0)) { confirm_["quantity"]=n; send(confirm_); }
            }
            else send(confirm_);
            confirm_=nullptr; split_amount_.clear();
        }
        else if (value=="confirm-no") { confirm_=nullptr; split_amount_.clear(); }
        else if (value=="system-close") system_menu_=0;
        else if (value=="system-settings") { system_menu_=0; prefs_saved_=options_.prefs; toggle_window(win_settings,true); }
        else if (value=="system-quit") { system_menu_=0; confirm_="quit"; confirm_text_="Oyundan çıkmak istediğinize eminmisiniz?"; }
        else if (value.starts_with("pref:")) { setting(value[5]-'0')=static_cast<float>(std::stoi(value.substr(7)))/10; apply_prefs(); }
        else if (value=="prefs-keep") toggle_window(win_settings);
        else if (value=="upscaler-next" || value.starts_with("fsr-sharp:") || value=="framegen-toggle") {
            // Off -> FSR 3.1.4 Native AA -> Quality -> Balanced -> Performance -> Ultra Performance -> Off.
            if (value=="upscaler-next") {
                const auto next=options_.upscaler=="fsr3"?static_cast<std::size_t>(fsr_quality(options_.fsr_mode))+1:0;
                options_.upscaler=next<fsr_modes.size()?"fsr3":"bilinear";
                if (next<fsr_modes.size()) options_.fsr_mode=fsr_modes[next];
                else options_.frame_generation=false; // saved bilinear+frame generation would be unstartable
            } else if (value=="framegen-toggle") { if (options_.upscaler=="fsr3") options_.frame_generation=!options_.frame_generation; }
            else options_.fsr_sharpness=static_cast<double>(std::stoi(value.substr(10)))/10;
            renderer_->set_upscaling(options_.upscaler=="fsr3"?ikv::Upscaler::fsr3:ikv::Upscaler::bilinear,
                fsr_quality(options_.fsr_mode),static_cast<float>(options_.fsr_sharpness),options_.frame_generation);
        }
        else if (value=="prefs-revert") { options_.prefs=prefs_saved_; apply_prefs(); toggle_window(win_settings); }
        else if (value=="vendor") toggle_window(win_vendor,true);
        else if (value.starts_with("buy:")) { // 00431700: client checks in order, notice (system colour) on the first failure; Akçe goods confirm first
            const auto item=value.substr(4);
            const Json& st=state_.contains("character")?state_.at("character").value("state",Json::object()):empty_object();
            const auto& items=state_.contains("catalogs")?state_.at("catalogs").value("items",Json::object()):empty_object();
            const Json d=items.contains(item)?items.at(item):Json::object();
            const Json buy{{"action","buy"},{"target",target_},{"item",item},{"quantity",1}};
            const bool guild=st.contains("guild") && st.at("guild").is_object();
            if (st.value("currency",int64_t(0))<d.value("price",int64_t(0))) notice("Bu eşyayı satın almak için yeterli paranız yok.");
            else if (st.value("clan_currency",int64_t(0))<d.value("clan_price",int64_t(0))) notice("Bu eşyayı satın almak için yeterli KLAN paranız yok.");
            else if (d.contains("guild_level") && !guild) notice("Bu eşyayı satın almak için bir loncanız olmalı.");
            else if (d.contains("guild_level") && st.at("guild").value("level",1)<d.at("guild_level").get<int>()) notice("Bu eşyayı satın almak için lonca seviyeniz yetersiz.");
            else if (st.value("akce",int64_t(0))<d.value("akce_price",int64_t(0))/5) notice("Bu eşyayı satın almak için yeterli Akçeniz yok."); // trunc(akceprice·0.2)
            else if (d.contains("akce_price")) ask(buy,"Satın almak istediğinizden emin misiniz?"); // 004b8230 box, 0x591050
            else send(buy);
        }
        else if (value=="map-toggle") map_open_=!map_open_;
        else if (value=="bank-open") send(target_action("bank_open")); // the window opens when the server reports the session
        else if (value=="craft-page+" || value=="craft-page-") { craft_page_=(craft_page_+(value.back()=='+'?1:5))%6; scroll_[win_craft]=0; }
        else if (value=="craft-scroll+") ++scroll_[win_craft]; // clamped when painted
        else if (value=="craft-scroll-") scroll_[win_craft]=std::max(0,scroll_[win_craft]-1);
        else if (value.starts_with("recipe:")) craft_selected_=value.substr(7);
        else if (value=="craft-make" || value=="craft-forget") {
            if (craft_selected_.empty()) notice("Üretebileceğiniz birşey seçili değil"); // original 0x592914
            else if (value=="craft-make") send(Json{{"action","craft"},{"recipe",craft_selected_}});
            else ask(Json{{"action","recipe_forget"},{"recipe",craft_selected_}},"Bu üretim reçetesi hafızanızdan kalıcı olarak silinecektir. Emin misiniz?"); // 0x590f40
        }
        else if (value.starts_with("bank-tab:")) { // 00440110/004401d0: ← only above tab 0, → only below tab 1
            const int tab=bank_tab()+(value.back()=='+'?1:-1);
            if (tab>=0 && tab<=1 && state_.contains("bank")) { send(Json{{"action","bank_tab"},{"tab",tab}}); bank_selected_.clear(); }
        }
        else if (value.starts_with("bank-item:")) bank_selected_=value.substr(10);
        else if (value=="bank-take") {
            for (const auto& entry:state_.value("bank",Json::object()).value("items",Json::array()))
                if (entry.value("uid",std::string{})==bank_selected_) send(Json{{"action","bank_withdraw"},{"uid",bank_selected_},{"quantity",entry.value("quantity",1)}});
            bank_selected_.clear();
        }
        else if (value=="trade") { // 004b3b20: 'Ngrt' unless already trading (0x59b498); the window opens with the session
            if (trade_.active()) notice("Alışveriş halindeyken başka alışveriş başlatamazsınız.");
            else send(target_action("trade_request"));
        }
        else if (value.starts_with("whisper:")) {
            const auto id=static_cast<std::uint32_t>(std::strtoul(value.c_str()+8,nullptr,10));
            whisper(id,name_of(id)); chat_menu_id_=0; editing_=true;
        }
        else if (value.starts_with("reflect:")) { // SNAY 00408ad0: the name goes into the chat input line
            input_=name_of(static_cast<std::uint32_t>(std::strtoul(value.c_str()+8,nullptr,10))); chat_menu_id_=0; editing_=true;
        }
        else if (value.starts_with("group-kick:")) {
            ask(Json{{"action","group_kick"},{"target",std::strtoul(value.c_str()+11,nullptr,10)}},"Bu oyuncuyu gruptan atmak istediğinize emin misiniz?"); // 0x58ea38
        }
        else if (value=="group-leave") ask(Json{{"action","group_leave"}},"Gruptan ayrılmak istediğinize emin misiniz?"); // 0x58e9e0
        else if (value=="toggle-trade") { // 0040d330 (A) and 0040d220 (G): flip, swap the button art, print the system line
            trade_allowed_=!trade_allowed_; notice(trade_allowed_?"Oyuncularla alışverişe izin verdiniz!":"Oyuncularla alışverişi kapattınız!");
        }
        else if (value=="toggle-invites") {
            invites_allowed_=!invites_allowed_;
            notice(invites_allowed_?"Size gelecek grup ve lonca davetlerine izin verdiniz!":"Size gelecek grup ve lonca davetlerini engellediniz!");
        }
        else if (value.starts_with("chatline:")) { // 0040aad0: clicking a player's chat line opens the name menu
            const auto index=std::strtoul(value.c_str()+9,nullptr,10);
            if (index<log_.size() && log_[index].from) { chat_menu_id_=log_[index].from; chat_menu_name_=log_[index].name; }
        }
        else if (value=="chat-menu-close") chat_menu_id_=0;
        else if (value.starts_with("loot-slot:") && loot_.is_object()) send(Json{{"action","loot_take"},{"target",loot_.at("target")},{"slot",std::strtoul(value.c_str()+10,nullptr,10)}});
        else if (value=="loot-close") loot_=nullptr; // 0042de30 hides the window
        else if (value=="send") { submit_input(); editing_=false; }
        else if (value=="chat+") chat_extra_=std::min(chat_extra_+1,4);
        else if (value=="chat-") chat_extra_=std::max(chat_extra_-1,0);
        else if (value=="chat-up") ++chat_scroll_;
        else if (value=="chat-down") chat_scroll_=std::max(0,chat_scroll_-1);
        else if (value=="chat-end") chat_scroll_=0;
        else if (value=="dialogue-") --dialogue_page_; // clamped when painted
        else if (value=="dialogue+") ++dialogue_page_;
        else if (value=="dialogue-close") dialogue_=nullptr;
        else if (value.starts_with("quest:")) { selected_quest_=value.substr(6); quest_text_scroll_=0; }
        else if (value=="quest-up") scroll_[win_quests]=std::max(0,scroll_[win_quests]-1);
        else if (value=="quest-down") ++scroll_[win_quests];
        else if (value=="quest-text-up") quest_text_scroll_=std::max(0,quest_text_scroll_-1);
        else if (value=="quest-text-down") ++quest_text_scroll_;
        else if (value=="quest-delete" && !selected_quest_.empty()) // 00449740: confirm 0x592f88, then Seçili görevi sil (00449720 → 00449600)
            ask(Json{{"action","quest_abandon"},{"quest",selected_quest_}},"Emin misiniz? (Otomatik gelen görevleri sildiğinizde sadece Mısır Çarşısındaki GB-Anı dan tekrar alabilirsiniz.)");
    }
    // Left click on the HUD; false when no HUD element is under (x, y).
    bool hud_click(float x,float y) {
        const auto hit=std::find_if(hits_.rbegin(),hits_.rend(),[&](const Hit& h){return inside({h.x,h.y,h.w,h.h},x,y);});
        if (hit==hits_.rend()) return false;
        const Hit chosen=*hit;
        // 00490120/004909a0: a button with a callback plays ui\click.raw.
        if (!chosen.action.is_null() || chosen.mode>=0 || !chosen.value.empty()) audio_->play_event("ui\\click.raw");
        if (chosen.value!="context") context_=0;
        if (!chosen.action.is_null()) send(chosen.action);
        else if (chosen.mode>=0) toggle_window(chosen.mode);
        else if (chosen.value.starts_with("target:")) { handle(chosen.value); click_entity(target_); } // a name label acts like its actor
        else handle(chosen.value);
        if (!chosen.value.starts_with("chatline:")) chat_menu_id_=0; // after handling: whisper/reflect still read its name
        hud_dirty_=true;
        return true;
    }
    std::pair<float,float> pixel(float x,float y) const {
        int ww=0,wh=0; SDL_GetWindowSize(window_,&ww,&wh);
        return {x*static_cast<float>(width_)/static_cast<float>(std::max(ww,1)),y*static_cast<float>(height_)/static_cast<float>(std::max(wh,1))};
    }
    static bool inside(const std::array<int,4>& r,float x,float y) {
        return x>=static_cast<float>(r[0]) && x<static_cast<float>(r[0]+r[2]) && y>=static_cast<float>(r[1]) && y<static_cast<float>(r[1]+r[3]);
    }
    // Ganimet sacks (classes.dat object 90002, items\id_item_box.adf) render under the corpse's id shifted below the NPC range.
    static constexpr std::uint32_t sack_offset=0x40000000u;
    // Original click model: a hostile is selected and attacked (the server approaches and auto-attacks);
    // a corpse or its Ganimet sack is walked to and looted.
    void click_entity(std::uint32_t id) {
        if (!entity(id) && id>=sack_offset && id<2*sack_offset) id+=sack_offset;
        target_=id;
        const Json* e=entity(id);
        if (!e) return;
        // talk cursor; an örs repairs; a <loc> door is used (original left click on a type-3 entity sends 'nchr', 004b6970).
        // A hostile Klan OOK (other clan) is attacked like a monster.
        const auto kind=e->value("kind",std::string{});
        const bool hostile=kind!="player" && relation(*e)==1; // monsters, other clans' banners and Klan OOK guards
        if (!hostile && (kind=="merchant" || kind=="questgiver" || kind=="anvil" || kind=="door" || kind=="reset" || (kind=="prop" && e->value("interactable",false)))) { send(target_action("interact")); return; }
        if (!hostile) return;
        if (e->value("alive",true)) send(target_action("attack"));
        else if (!e->value("looted",true) && e->value("owner",0u)==session_->self) send(target_action("loot"));
    }
    // 004b3ba0 hover pick: the cursor ray against each entity root's classes.dat box. 00479fd0 sets half extents
    // (boundx,boundy,boundz) centred at (0,boundy,0) above the feet and 0047cb80 probes it axis-aligned
    // (D3DXBoxBoundProbe). The original returns the first hit of its entity set; the nearest box entry is taken here.
    // Entities without a box (Ganimet sacks, unbound classes) keep the old ground-proximity rule. Never self.
    // ground receives the walkable ground hit when no box was hit.
    std::uint32_t entity_at(float x,float y,std::optional<std::array<float,3>>& ground) {
        if (width_<=0 || height_<=0) return 0;
        const auto [eye,ray]=camera_ray(renderer_->camera(),x,y,width_,height_);
        float nearest=INFINITY; std::uint32_t id=0;
        for (const auto& pose:poses_) {
            const auto* actor=pose.id!=session_->self?actor_state(pose.id):nullptr;
            if (!actor || actor->bounds[0]<=0 || actor->bounds[1]<=0 || actor->bounds[2]<=0) continue;
            const auto& b=actor->bounds;
            const auto t=ray_box(eye,ray,{pose.x-b[0],pose.ground_y,pose.z-b[2]},{pose.x+b[0],pose.ground_y+2*b[1],pose.z+b[2]});
            if (t && *t<nearest) { nearest=*t; id=pose.id; }
        }
        if (id) return id;
        ground=pick_scene(scene_,renderer_->camera(),x,y,width_,height_);
        if (!ground) return 0;
        float best=.75f*.75f;
        for (const auto& pose:poses_) if (pose.id!=session_->self) {
            const float dx=(*ground)[0]-pose.x,dz=(*ground)[2]-pose.z;
            if (dx*dx+dz*dz<best) { best=dx*dx+dz*dz; id=pose.id; }
        }
        return id;
    }
    void pick(float x,float y) {
        if (const auto id=label_at(x,y); id && id!=session_->self) { click_entity(id); return; }
        std::optional<std::array<float,3>> hit;
        if (const auto id=entity_at(x,y,hit)) { click_entity(id); return; }
        // The original simply ignores a click that hits no walkable ground; no player-facing message.
        if (!hit) return;
        const double wx=std::round(static_cast<double>((*hit)[0])*1000),wy=std::round(static_cast<double>((*hit)[2])*1000);
        const auto& bounds=state_.at("bounds");
        if (wx<bounds[0].get<double>() || wy<bounds[1].get<double>() || wx>bounds[2].get<double>() || wy>bounds[3].get<double>()) {
            return;
        }
        if (!ikv::zone_height(scene_.zone,static_cast<float>(wx/1000),static_cast<float>(wy/1000))) return;
        session_->destination(static_cast<std::int32_t>(wx),static_cast<std::int32_t>(wy));
        if (effects_) effects_->trigger_at("Click",*hit); // 004b6524: the original "Click" system marks the clicked ground point
    }
    // Money as 00436bd0 prints it: '.' between thousands.
    static std::string dotted(std::uint64_t value) {
        auto text=std::to_string(value);
        for (auto at=static_cast<std::ptrdiff_t>(text.size())-3;at>0;at-=3) text.insert(static_cast<std::size_t>(at),1,'.');
        return text;
    }
    // Original Alışveriş (0043b8e0, VOD 42:30): UI2 0,304 240x196 at ((W-304)/2,160) with "Alışveriş" (0x592618, white) on the black
    // plate, × 219,7 ('Fqrt', 0042e130), own 3x2 cells 16+34c,27+34r and the peer's at +128, "Para: %s" 16,100 / 128,100, −/+ 17,117 /
    // 34,117, names 16,142 / 128,142 and the ✓ strips 0,143 130x50 / 130,143 90x50 whose lit art lies at UI2 0,503 / 130,503 (shown
    // per side once accepted: 004a8940 states 2/4/5, 0042e9e0). Offers beyond six (native server limit 32) scroll by rows of three;
    // ✓ needs every offered cell painted (ClientTrade presented rule, completed in draw()).
    void paint_trade(int x,int y) {
        trade_pending_rows_=0; trade_pending_present_=false;
        const float fx=static_cast<float>(x),fy=static_cast<float>(y);
        const auto& terms=trade_.offer();
        const auto& own=terms.at("own"); const auto& other=terms.at("other");
        const auto& mine=own.at("items"); const auto& theirs=other.at("items");
        ui_atlas(original_ui2_,fx,fy,0,0,240,196,0,304,240,196);
        if (own.at("accepted")==true) ui_atlas(original_ui2_,fx,fy,0,143,130,50,0,503,130,50);
        if (other.at("accepted")==true) ui_atlas(original_ui2_,fx,fy,130,143,90,50,130,503,90,50);
        hits_.push_back({x,y,S(240),S(196),nullptr,-1,{}});
        window_rects_.push_back({win_trade,{x,y,S(240),S(196)}});
        hits_.push_back({x+S(219),y+S(7),S(16),S(16),Json{{"action","trade_cancel"}},-1,{}});
        hits_.push_back({x+S(17),y+S(117),S(16),S(16),nullptr,-1,"currency-"});
        hits_.push_back({x+S(34),y+S(117),S(16),S(16),nullptr,-1,"currency+"});
        hits_.push_back({x,y+S(143),S(130),S(50),nullptr,-1,"trade-accept"});
        const int px=ui_->pixel_size(); ui_->set_pixel_size(small_px()); // VOD 42:30: title and labels share one size
        label(x+S(20),y+S(11)-lh()/2,"Alışveriş",white_rgba);
        const auto line=[&](float lx,float ly,const std::string& text){ label(x+S(lx),y+S(ly)+(S(15)-lh())/2,fit(text,S(96)),white_rgba); };
        line(16,100,"Para: "+dotted(own.at("currency").get<std::uint64_t>()));
        line(128,100,"Para: "+dotted(other.at("currency").get<std::uint64_t>()));
        line(16,142,state_.contains("character")?state_.at("character").value("name",std::string{}):std::string{});
        line(128,142,terms.at("peer_name").get<std::string>());
        auto& scroll=scroll_[win_trade];
        const auto count=std::max(mine.size(),theirs.size());
        scroll=std::clamp(scroll,0,std::max(0,static_cast<int>((count+2)/3)-2));
        float mouse_x=0,mouse_y=0; SDL_GetMouseState(&mouse_x,&mouse_y);
        const auto [mx,my]=pixel(mouse_x,mouse_y);
        const Json* hovered=nullptr; int hover_x=0,hover_y=0;
        const bool locked=own_trade_locked();
        for (int cell=0;cell<6;++cell) {
            const auto index=static_cast<std::size_t>(3*scroll+cell);
            for (int side=0;side<2;++side) {
                const auto& items=side?theirs:mine;
                if (index>=items.size()) continue;
                const auto& item=items[index];
                const int ix=x+S(static_cast<float>(16+34*(cell%3)+128*side)),iy=y+S(static_cast<float>(27+34*(cell/3)));
                source_icon(static_cast<float>(ix),static_cast<float>(iy),32*s_,item_icon(item.at("item").get<std::string>()));
                if (item.at("quantity").get<std::uint64_t>()>1) label(ix+S(2),iy+S(32)-lh(),std::to_string(item.at("quantity").get<std::uint64_t>()));
                if (!side && !locked) { // back onto the bag; a drag source must be the topmost hit
                    hits_.push_back({ix,iy,S(32),S(32),nullptr,-1,{}});
                    drags_.push_back({{ix,iy,S(32),S(32)},Json{{"item",item.at("uid")},{"trade",true}}});
                }
                if (inside({ix,iy,S(32),S(32)},mx,my)) { hovered=&item; hover_x=ix; hover_y=iy; }
            }
            if (index<count) trade_pending_rows_|=std::uint64_t{1}<<index;
        }
        if (hovered) {
            const auto text=hovered->at("name").get<std::string>()+(hovered->at("quantity").get<std::uint64_t>()>1?" x"+std::to_string(hovered->at("quantity").get<std::uint64_t>()):std::string{});
            const int tip_w=ui_->text_width(text)+S(16),tip_x=std::clamp(hover_x,0,std::max(0,width_-tip_w)),tip_y=hover_y-lh()-S(10);
            ui_->rect(tip_x,tip_y,tip_w,lh()+S(8),{8,8,12,230}); outline(tip_x,tip_y,tip_w,lh()+S(8),{150,130,90,255});
            ui_->text(tip_x+S(8),tip_y+S(4),text,white_rgba);
        }
        ui_->set_pixel_size(px);
        trade_painted_revision_=trade_.revision(); trade_pending_present_=true;
    }
    void draw(float dt) {
        // Before the first authoritative scene the loading screen stands in for the world (VOD 02:12).
        if (!scene_.source_registry_id && session_ && !disconnected_ && !quit_) { begin_loading(); paint_loading(); }
        if (!scene_.source_registry_id || !original_ui_cached_) return;
        const int old_width=width_,old_height=height_;
        if (!SDL_GetWindowSizeInPixels(window_,&width_,&height_) || width_<=0 || height_<=0) return;
        if (width_!=old_width || height_!=old_height) { renderer_->resize(width_,height_); hud_dirty_=true; }
        // Original HUD is authored for 1080p; scale its atlas pixels and Verdana size with the drawable height.
        s_=std::clamp(static_cast<float>(height_)/1080.f,.6f,2.f);
        if (const int px=std::max(11,static_cast<int>(std::lround(16*s_))); !ui_ || px!=ui_px_) {
            ui_=std::make_unique<ikv::ClientUI>(scene_.font,px); ui_px_=px; hud_dirty_=true;
        }
        rendered_.clear();
        const double render_tick=tick_epoch_?std::chrono::duration<double>(Clock::now()-*tick_epoch_).count()*20-interpolation_ticks:0;
        const auto place=[&](ikv::Player p) {
            if (auto* actor=actor_state(p.id); actor && actor->samples) {
                const auto& h=actor->history; const auto n=actor->samples;
                std::size_t i=0; while (i+1<n && h[i+1].tick<=render_tick) ++i;
                // Past the newest sample, hold it but keep the last step's gait briefly so a late packet cannot flicker idle.
                const auto& b=h[i+1<n?i+1:i]; const auto& a=h[i+1<n||!i?i:i-1];
                const double span=std::max(1.0,double(b.tick)-double(a.tick));
                const double t=std::clamp((render_tick-a.tick)/span,0.0,1.0);
                p.x=static_cast<std::int32_t>(std::lround(a.x+(b.x-a.x)*t)); p.y=static_cast<std::int32_t>(std::lround(a.z+(b.z-a.z)*t));
                // Authority validated each sample, not every point between them (respawns within the teleport bound).
                if (!ikv::zone_height(scene_.zone,static_cast<float>(p.x)/1000.f,static_cast<float>(p.y)/1000.f)) { p.x=b.x; p.y=b.z; }
                const bool moving=(a.x!=b.x || a.z!=b.z) && render_tick<b.tick+late_gait_ticks;
                float speed=0;
                if (moving) {
                    // Source actor models face their local -Z; 0047aa80 heads along the current path segment.
                    actor->yaw=std::atan2(static_cast<float>(a.x-b.x),static_cast<float>(a.z-b.z));
                    speed=static_cast<float>(std::hypot(double(b.x-a.x),double(b.z-a.z))/(span*.05));
                }
                // 0047aa80 slerps toward that heading by min(1,10/FPS) each frame (00445090).
                actor->shown_yaw+=std::remainder(actor->yaw-actor->shown_yaw,6.2831853f)*std::min(1.f,10*dt);
                locomote(*actor,moving,speed);
            }
            rendered_.push_back(p);
        };
        if (session_) for (const auto& p:players_) place(p);
        for (const auto& p:entities_) {
            if (session_ && std::any_of(players_.begin(),players_.end(),[&](const auto& player){return player.id==p.id;})) continue;
            place(p);
        }
        poses_.clear();
        for (const auto& position:rendered_) {
            auto* actor=actor_state(position.id);
            if (!actor) continue;
            const float x=static_cast<float>(position.x)/1000.f,z=static_cast<float>(position.y)/1000.f;
            const auto ground=ikv::zone_height(scene_.zone,x,z);
            if (!ground) throw std::runtime_error("authoritative visible entity has no source-grid support");
            poses_.push_back({position.id,x,*ground,z,actor->shown_yaw});
            if (session_ && position.id==session_->self) {
                auto camera=renderer_->camera(); camera.target={x,*ground+6,z};
                camera.distance=options_.camera.distance; camera.distance=unoccluded_distance(scene_,camera); renderer_->set_camera(camera);
            }
        }
        // Ganimet sack (classes.dat object 90002 items\id_item_box.adf; VOD 16:30) beside each unlooted corpse.
        std::vector<std::uint32_t> sacks;
        if (session_ && state_.contains("entities")) for (const auto& e:state_.at("entities")) {
            if (e.value("kind",std::string{})!="enemy" || e.value("alive",true) || e.value("looted",true)) continue;
            const auto id=e.at("id").get<std::uint32_t>();
            const auto* corpse=pose(id);
            if (!corpse || id<2*sack_offset) continue;
            const float x=corpse->x+1,z=corpse->z;
            const auto ground=ikv::zone_height(scene_.zone,x,z);
            sacks.push_back(id-sack_offset); poses_.push_back({id-sack_offset,x,ground.value_or(corpse->ground_y),z,0});
        }
        if (sacks!=sacks_) {
            for (const auto id:sacks_) if (std::find(sacks.begin(),sacks.end(),id)==sacks.end()) ikv::detach_source_actor(scene_,id);
            for (const auto id:sacks) if (std::find(sacks_.begin(),sacks_.end(),id)==sacks_.end())
                ikv::attach_entity_model(scene_,options_.assets,ikv::source_object_member("items\\id_item_box.adf"),id);
            sacks_=std::move(sacks);
        }
        // Ölü Yok Olma Süresi (0047d06f): an NPC body (classid >= 8) disappears pref*30 s + 3 s after its death unless the
        // slider is at least .98; its Ganimet sack stays.
        if (const float corpse=options_.prefs[4]; corpse<.98f) {
            const auto limit=std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float>(corpse*30+3));
            const auto now=Clock::now();
            std::erase_if(poses_,[&](const auto& p) {
                const auto* actor=actor_state(p.id);
                return actor && !actor->alive && !actor->node && !is_pc(*actor) && now-actor->died_at>limit;
            });
        }
        audio_->frame(scene_,poses_,session_?session_->self:0,ikv::camera_basis(renderer_->camera()));
        update_labels();
        hud_frame();
        if (!flash_.empty() && Clock::now()>=flash_until_) { flash_.clear(); hud_dirty_=true; }
        if (Clock::now()<cooldown_until_+std::chrono::milliseconds(300) && Clock::now()>=cooldown_repaint_) { hud_dirty_=true; cooldown_repaint_=Clock::now()+std::chrono::milliseconds(250); }
        if (dead()) {
            const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(resurrect_at_-Clock::now()).count();
            if (const long long shown=ms<1?-1:ms/1000; shown!=resurrect_shown_) { resurrect_shown_=shown; hud_dirty_=true; }
        }
        if (hud_dirty_) paint_hud();
        else if (world_dirty_) { paint_world(); present_hud(); }
        if (effects_) {
            effects_->update(dt,poses_);
            for (const auto& actor:actors_) if (!actor.shadow.empty() && actor.shadow!="none") effects_->attach(actor.shadow,actor.id);
            // 004315c0 (WEAR 0x4a94d0): the slot-11 weapon (004301b0 finds the worn item with slot 0xb) attaches its two prefixes'
            // prefixes.dat effect_name (wp_* weapon_gfx_psystem, 00495810) to the weapon's render node while its Eskime is above
            // zero (the server only sends prefixes then); armour prefixes carry no glow. Taking it off detaches it.
            for (const auto& actor:actors_) {
                std::vector<std::string> want;
                for (const auto& item:actor.binding.is_object()?actor.binding.value("equipment",Json::array()):Json::array()) if (item.value("slot",0)==11)
                    for (const auto& p:item.value("prefixes",Json::array()))
                        if (const auto f=source_prefixes.find(p.get<int>()); f!=source_prefixes.end() && !f->second.effect.empty()) want.push_back(f->second.effect);
                auto& had=glows_[actor.id];
                for (const auto& name:had) if (std::find(want.begin(),want.end(),name)==want.end()) effects_->detach(name,actor.id);
                for (const auto& name:want) effects_->attach(name,actor.id,{1,1,1,1},true);
                had=std::move(want);
            }
            // 0041b090 attaches GroundHilite to the selected entity tinted by relation 00478090 (1 red, 2 yellow, 3 green);
            // 00417ee0 detaches it from the previous one.
            const auto* selected=session_ && target_!=session_->self?entity(target_):nullptr;
            if (const auto ring=selected?target_:0; ring!=hilite_) { if (hilite_) effects_->detach("GroundHilite",hilite_); hilite_=ring; }
            if (selected) {
                static constexpr std::array<std::array<float,4>,4> tints{{{1,1,1,1},{1,0,0,1},{1,1,0,1},{0,1,0,1}}};
                const auto r=relation(*selected);
                effects_->attach("GroundHilite",target_,tints[static_cast<std::size_t>(r>=1 && r<=3?r:0)]);
            }
            if (session_ && std::any_of(poses_.begin(),poses_.end(),[&](const auto& p){return p.id==session_->self;})) {
                for (const auto& name:scripted_effects_) effects_->trigger(name,session_->self);
                scripted_effects_.clear();
            }
            // animation.dat visual_effect fires with the clip cue (same 0045c980/0045cb9e points as clip audio).
            for (const auto& pose:poses_) {
                const auto* controller=actor_controller(pose.id);
                if (!controller) continue;
                auto [cue,inserted]=effect_cues_.try_emplace(pose.id,controller->audio_cue);
                if (inserted || cue->second==controller->audio_cue) continue;
                cue->second=controller->audio_cue;
                if (controller->library>=scene_.actor_tables.libraries.size()) continue;
                const auto& clips=scene_.actor_tables.libraries[controller->library].clips;
                if (controller->current_clip<clips.size() && !clips[controller->current_clip].visual_effect.empty() &&
                    clips[controller->current_clip].visual_effect!="none")
                    effects_->trigger(clips[controller->current_clip].visual_effect,pose.id);
            }
            renderer_->set_particles(effects_->frame(renderer_->camera()));
        }
        tutorial();
        renderer_->set_grayscale(dead()); // VOD 34:00: the dead player's world view is grayscale, HUD stays in colour
        // 004b3ba0 hover pick: the single entity under the cursor gets the contour shell, red for actors (473f17) and green for
        // the Ganimet box (4a9c3b); never self, dead hostiles or while Alt is held (DIK_LMENU/RMENU 0x5d7eb1/0x5d7f31).
        const bool alt=(SDL_GetModState()&SDL_KMOD_ALT)!=0;
        for (auto& pose:poses_) {
            pose.contour={};
            if (alt || !hover_ || pose.id!=hover_ || (session_ && pose.id==session_->self)) continue;
            if (pose.id>=sack_offset && pose.id<2*sack_offset) { pose.contour={0,1,0}; continue; }
            const Json* e=entity(pose.id);
            if (e && !(e->value("kind",std::string{})=="enemy" && !e->value("alive",true))) pose.contour={1,0,0};
        }
        // Next frame's update_scene skips actors out of view; one that came into view now is skinned before it is drawn.
        if (hold_offscreen_skins()) ikv::update_scene(scene_,elapsed_);
        renderer_->render(scene_,poses_,session_?session_->self:0,dt);
        if (trade_pending_present_) {
            if (trade_.active() && trade_.revision()==trade_painted_revision_) {
                trade_seen_rows_|=trade_pending_rows_;
                const auto& terms=trade_.offer();
                const auto count=std::max(terms.at("own").at("items").size(),terms.at("other").at("items").size());
                const auto needed=count?(std::uint64_t{1}<<count)-1:0;
                if ((trade_seen_rows_&needed)==needed) trade_.mark_presented(trade_painted_revision_);
            }
            trade_pending_present_=false;
        }
        if (!screenshot_request_.empty()) {
            try { renderer_->screenshot(screenshot_request_); }
            catch (const std::exception& error) { std::cerr<<"screenshot "<<screenshot_request_<<" failed: "<<error.what()<<'\n'; }
            screenshot_request_.clear();
        }
        if (quit_ && !captured_ && !options_.screenshot.empty() && session_ && has_snapshot_ &&
            session_->drained() && !pending_presentation()) {
            renderer_->screenshot(options_.screenshot); captured_=true;
        }
    }
    void save_settings() {
        if (options_.settings.empty()) return;
        if (!options_.settings.parent_path().empty()) std::filesystem::create_directories(options_.settings.parent_path());
        int w=0,h=0; SDL_GetWindowSize(window_,&w,&h);
        w=std::clamp(w,640,3840); h=std::clamp(h,480,2160);
        const auto& c=renderer_->camera();
        Json settings{{"width",w},{"height",h},{"fullscreen",options_.fullscreen},{"volume",options_.volume},{"music_volume",options_.music_volume},{"renderer",options_.renderer},
            {"appearance",options_.appearance},{"rt",options_.rt},{"rt_style",options_.rt_style},{"render_scale",options_.render_scale},
            {"upscaler",options_.upscaler},{"fsr_mode",options_.fsr_mode},{"fsr_sharpness",options_.fsr_sharpness},
            {"frame_generation",options_.frame_generation},{"texture_detail",options_.texture_detail},
            {"lightmap_detail",options_.lightmap_detail},{"preferences",options_.prefs},
            {"camera",{{"yaw",c.yaw},{"pitch",c.pitch},{"distance",options_.camera.distance}}}};
        const auto temp=options_.settings.string()+".tmp";
        std::ofstream file(temp,std::ios::trunc); file << settings.dump(2) << '\n'; file.close();
        if (!file) throw std::runtime_error("settings write failed");
        std::filesystem::rename(temp,options_.settings);
    }
};
}
int main(int argc,char** argv) {
    try {
        const auto options=parse_options(argc,argv);
        ikv::set_texture_detail(static_cast<unsigned>(options.texture_detail),static_cast<unsigned>(options.lightmap_detail));
#if !IKV_HAS_VULKAN
        if (options.renderer=="vulkan")
            throw std::runtime_error("Vulkan renderer is unavailable in this OpenGL-only build; rebuild with IKV_BUILD_VULKAN=ON");
#endif
        std::signal(SIGINT,interrupt_handler); std::signal(SIGTERM,interrupt_handler); std::signal(SIGPIPE,SIG_IGN);
        Secret secret;
        if (options.headless) return headless(options,secret);
        Application app(options,secret); return app.run();
    } catch (const std::exception& e) {
        std::cerr << "ikv-client: " << e.what() << '\n'; return 1;
    }
}
