#include "ikv/scene.hpp"
#include <boost/property_tree/xml_parser.hpp>
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <fstream>
#include <initializer_list>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace ikv {
namespace {
constexpr std::size_t max_file = 1024 * 1024;
constexpr std::size_t max_text = 2 * 1024 * 1024;
constexpr std::size_t max_pages = 1024;
constexpr std::size_t max_paragraphs = 8192;
using Tree = boost::property_tree::ptree;
constexpr std::size_t max_table_file = 4 * 1024 * 1024;
constexpr std::size_t max_table_nodes = 100000;
constexpr std::size_t max_actor_string = 4096;
std::vector<HelpPage> decode_help(std::string_view bytes);
ActorTables decode_actor_tables(std::string classes, std::string animation);
ActorAnimation bind_actor_animation(const ActorTables& tables, const std::string& member,
                                   const SourceActorRecord* selected = nullptr);
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
}

void scene_resources_self_test() {
    const auto pages = decode_help("<?xml version='1.0' encoding='iso-8859-9'?>"
        "<Library><Book name='Kitap'><Page header='\xd0\xdd\xde\xf0\xfd\xfe'>"
        "<Paragraph text='  T\xfcrk\xe7" "e &amp; &#351;' color='4294915071'/>"
        "<Paragraph text='' /></Page></Book></Library>");
    require(pages.size() == 1 && pages[0].book == "Kitap", "help book decoding failed");
    require(pages[0].header == "\xc4\x9e\xc4\xb0\xc5\x9e\xc4\x9f\xc4\xb1\xc5\x9f",
            "ISO-8859-9 Turkish mapping failed");
    require(pages[0].paragraphs.size() == 2 &&
            pages[0].paragraphs[0].text == "  T\xc3\xbcrk\xc3\xa7" "e & \xc5\x9f" &&
            pages[0].paragraphs[0].color == 4294915071u &&
            pages[0].paragraphs[1].color == 0xffffffffu, "help paragraph decoding failed");
    for (const auto invalid : {"<Library><Book name='x'></Library>",
         "<!DOCTYPE Library SYSTEM 'file:///etc/passwd'><Library/>",
         "<Library><Book name='&unknown;'/></Library>",
         "<Library><Book name='x'><Page header='y'><Paragraph text='x' color='4294967296'/></Page></Book></Library>"}) {
        bool rejected = false;
        try { (void)decode_help(invalid); } catch (const std::exception&) { rejected = true; }
        require(rejected, "invalid help XML accepted");
    }
    const auto encoded = [](std::string bytes) {
        for (std::size_t i = 0; i < bytes.size(); ++i)
            bytes[i] = static_cast<char>(static_cast<unsigned char>(bytes[i]) ^ ((i * 0x57 + 0x55) & 255) ^ 0xaa);
        return bytes;
    };
    const std::string classes = "<?xml version='1.0' encoding='iso-8859-9'?>"
        "<classes><group><npc path='npc\\fixture.adf' meshid='Raw_Model' classid='016' biped='Raw_Biped'/>"
        "<npc path='NPC/fixture.adf' meshid='Raw_Model' classid='016' biped='Raw_Biped'/></group></classes>";
    const std::string animation = "<?xml version='1.0' encoding='iso-8859-9'?>"
        "<animationdata><cliplist frommodel='Raw_Model' classid='016' defaultclip='0'>"
        "<clip clipid='1' name='Wait' start='10' end='70' loopstart='10' loops='true' jumptoid='1' jumptoframe='10'/>"
        "<clip clipid='2' name='Hail' start='75' end='135' loopstart='75' loops='false' jumptoid='1' jumptoframe='10'/>"
        "</cliplist></animationdata>";
    const auto tables = decode_actor_tables(encoded(classes), encoded(animation));
    const auto actor = bind_actor_animation(tables, "Objects/NPC/FIXTURE.adf");
    require(tables.bindings.size() == 1 && actor.library == 0 && !actor.stopped &&
            tables.libraries[0].class_id == "016" && tables.bindings[0].biped == "Raw_Biped" &&
            tables.libraries[0].clips.size() == 2, "actor XOR binding or raw key preservation failed");
    const auto metadata = decode_actor_tables(encoded(
        "<classes><group>"
        "<object uid='4999' path='Objects\\pc\\e_efe\\E_BODY_EFE.ADF' meshid='ID_E_BODY_EFE'"
        " classid='3' biped='BIP_E_BODY_EFE' objecttype='bodyroot' suitableslot='0'/>"
        "<npc uid='903' meshid='ID_E_BODY_EFE' classid='3'"
        " bodypiecelist='ID_E_BODY_EFE;ID_E_BST_MEL;ID_E_GVD_STD_EFE;ID_E_BCK_STD_EFE;ID_E_ELL_STD_EFE;ID_E_AYK_STD;ID_E_SAC_001;'/>"
        "<object uid='3001' path='pc\\e_hair\\ID_E_SAC_001_SIY.ADF'"
        " objecttype='bodypiece' suitableslot='6'/>"
        "<object uid='5000' path='pc/e_efe/e_body_efe.adf' meshid='ID_E_BODY_EFE'"
        " classid='3' biped='BIP_E_BODY_EFE' objecttype='bodyroot' suitableslot='0'/>"
        "<object uid='13014' objecttype='weaponlight' suitableslot='11' icon='75' pose='6'/>"
        "</group></classes>"), encoded(animation));
    require(metadata.source_records.size() == 5 && metadata.bindings.size() == 2 &&
            metadata.libraries.size() == tables.libraries.size() &&
            metadata.libraries[0].class_id == tables.libraries[0].class_id &&
            metadata.libraries[0].clips.size() == tables.libraries[0].clips.size(),
            "source metadata collection changed actor binding deduplication or libraries");
    const auto& body = metadata.source_records[0];
    const auto& npc = metadata.source_records[1];
    const auto& piece = metadata.source_records[2];
    require(body.uid == 4999 && body.tag == "object" && body.object_type == "bodyroot" &&
            body.path == "pc/e_efe/e_body_efe.adf" && body.model == "ID_E_BODY_EFE" &&
            body.class_id == "3" && body.biped == "BIP_E_BODY_EFE" &&
            body.bodypieces.empty() && body.suitable_slot == "0" && body.icon == -1 && body.pose == 0 &&
            npc.uid == 903 && npc.tag == "npc" && npc.path.empty() &&
            npc.model == body.model && npc.class_id == body.class_id &&
            npc.bodypieces == "ID_E_BODY_EFE;ID_E_BST_MEL;ID_E_GVD_STD_EFE;ID_E_BCK_STD_EFE;ID_E_ELL_STD_EFE;ID_E_AYK_STD;ID_E_SAC_001;" &&
            piece.uid == 3001 && piece.tag == "object" && piece.object_type == "bodypiece" &&
            piece.path == "pc/e_hair/id_e_sac_001_siy.adf" && piece.suitable_slot == "6" &&
            metadata.source_records[3].uid == 5000 &&
            metadata.source_records[4].uid == 13014 &&
            metadata.source_records[4].object_type == "weaponlight" &&
            metadata.source_records[4].suitable_slot == "11" &&
            metadata.source_records[4].icon == 75 && metadata.source_records[4].pose == 6,
            "source BODY/NPC identity, raw equipment strings or normalized member path lost");
    require(source_object_member(body.path) == "objects/pc/e_efe/e_body_efe.adf" &&
            source_object_member("objects\\PC\\E_EFE\\E_BODY_EFE.ADF") == source_object_member(body.path),
            "source object consumer did not resolve relative and prefixed members identically");
    for (const auto* unsafe : {"../pc/body.adf", "/pc/body.adf", "C:\\pc\\body.adf", "objects/../pc/body.adf"}) {
        bool rejected = false;
        try { (void)source_object_member(unsafe); } catch (const std::exception&) { rejected = true; }
        require(rejected, "unsafe source object member accepted");
    }
    Scene metadata_scene;
    metadata_scene.actor_tables = metadata;
    require(source_actor_record(metadata_scene, 13014).icon == 75 &&
            source_actor_record(metadata_scene, 13014).pose == 6,
            "selected source actor icon/pose consumer lost UID 13014");
    for (const auto* invalid_attribute : {"icon=''", "icon='75x'", "pose='6x'", "pose='2147483648'"}) {
        const auto invalid = decode_actor_tables(encoded(std::string("<classes><object uid='13014' ") +
            invalid_attribute + "/></classes>"), encoded(animation));
        Scene invalid_scene;
        invalid_scene.actor_tables = invalid;
        bool rejected = false;
        try { (void)source_actor_record(invalid_scene, 13014); }
        catch (const std::exception&) { rejected = true; }
        require(rejected && invalid.source_records.size() == 1 && !invalid.source_records[0].error.empty() &&
                invalid.source_records[0].icon == -1 && invalid.source_records[0].pose == 0 &&
                invalid.libraries.size() == tables.libraries.size(),
                "malformed source icon/pose metadata lost provenance or poisoned unrelated libraries");
    }
    struct SkillFixture {
        std::filesystem::path path;
        ~SkillFixture() { std::error_code error; std::filesystem::remove_all(path, error); }
    };
    const auto fixture_path = std::filesystem::temp_directory_path() /
        ("ikv-source-skills-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    require(std::filesystem::create_directory(fixture_path), "cannot create source skill fixture directory");
    const SkillFixture fixture{fixture_path};
    const auto skill_base = fixture.path / "base";
    std::filesystem::create_directories(skill_base / "data");
    const auto write_skills = [&](const std::string& xml) {
        const auto bytes = encoded(xml);
        std::ofstream stream(skill_base / "data" / "skills.dat", std::ios::binary | std::ios::trunc);
        stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        stream.close();
        require(static_cast<bool>(stream), "cannot write source skill fixture");
    };
    write_skills("<skills><skill classindex='0' skillno='0' hasicon='2' icon='0' detail='&amp;'/>"
        "<skill classindex='1' skillno='15' hasicon='1' icon='75'/>"
        "<skill classindex='2' skillno='15' hasicon='0' icon='89'/>"
        "<skill classindex='2' skillno='29'/></skills>");
    const auto skills = load_source_skills(fixture.path);
    require(skills.size() == 4 && skills[0].classindex == 0 && skills[0].skillno == 0 &&
            skills[0].hasicon && skills[0].icon == 0 &&
            skills[1].classindex == 1 && skills[1].skillno == 15 && skills[1].icon == 75 &&
            skills[2].classindex == 2 && skills[2].skillno == 15 && !skills[2].hasicon &&
            skills[2].icon == 89 && skills[3].skillno == 29 && !skills[3].hasicon && skills[3].icon == -1,
            "encrypted source skill metadata, absent icon or distinct raw singleton identities lost");
    require(load_source_skills(skill_base).size() == skills.size(), "direct source skill base not resolved");
    std::filesystem::create_directory(fixture.path / "app");
    std::filesystem::rename(skill_base, fixture.path / "app" / "base");
    const auto app_skills = load_source_skills(fixture.path);
    require(app_skills.size() == skills.size(), "app/base source skill root not resolved");
    std::filesystem::rename(fixture.path / "app" / "base", skill_base);
    for (const auto* attributes : {"classindex='3' skillno='0'", "classindex='-1' skillno='0'",
         "classindex='0' skillno='30'", "classindex='0' skillno='-1'",
         "classindex='0' skillno='15x'", "classindex='0' skillno='0' icon=''",
         "classindex='0' skillno='0' icon='2147483648'", "classindex='0' skillno='0' hasicon='true'"}) {
        write_skills(std::string("<skills><skill ") + attributes + "/></skills>");
        bool rejected = false;
        try { (void)load_source_skills(fixture.path); } catch (const std::exception&) { rejected = true; }
        require(rejected, "malformed or out-of-bounds source skill numeric metadata accepted");
    }
    for (const auto& xml : {
         std::string("<skills><skill classindex='0' skillno='0'/><skill classindex='0' skillno='0'/></skills>"),
         std::string("<source><skill classindex='0' skillno='0'/></source>"),
         std::string("<!DOCTYPE skills><skills/>"),
         std::string("<skills><unused field='1' field='2'/></skills>"),
         std::string("<skills><unused field='&unknown;'/></skills>"),
         std::string("<skills/>\0suffix", 16), std::string(max_table_file + 1, 'x'),
         std::string("<skills><unused field='") + std::string(max_actor_string + 1, 'x') + "'/></skills>"}) {
        write_skills(xml);
        bool rejected = false;
        try { (void)load_source_skills(fixture.path); } catch (const std::exception&) { rejected = true; }
        require(rejected, "duplicate identity or unsafe unselected source skill XML accepted");
    }
    const auto& clip = tables.libraries[0].clips[1];
    require(clip.id == 2 && clip.start == 75.0f * 0.033333335f &&
            clip.end == 135.0f * 0.033333335f && clip.loop_start == clip.start &&
            clip.loop_end == clip.end && clip.next_id == 1 && clip.next_frame == 10,
            "source actor clip fields or defaults failed");
    auto ambiguous = tables;
    ambiguous.bindings.push_back({"npc/fixture.adf", "Other_Model", "016", "Raw_Biped"});
    for (const auto& member : {"../npc/fixture.adf", "/npc/fixture.adf", "C:\\npc\\fixture.adf",
                               "npc/missing.adf", "npc/fixture.adf"}) {
        bool rejected = false;
        try { (void)bind_actor_animation(ambiguous, member); } catch (const std::exception&) { rejected = true; }
        require(rejected, "missing, unsafe or ambiguous actor binding accepted");
    }
    auto duplicate_library = tables;
    duplicate_library.libraries.push_back(tables.libraries[0]);
    bool duplicate_rejected = false;
    try { (void)bind_actor_animation(duplicate_library, "npc/fixture.adf"); }
    catch (const std::exception&) { duplicate_rejected = true; }
    require(duplicate_rejected, "duplicate actor library accepted");
    const auto defaults = decode_actor_tables(encoded(classes), encoded(
        "<animationdata><cliplist frommodel='Raw_Model' classid='016'>"
        "<clip clipid='2' end='1' jumptoid='2' jumptoframe='0'/>"
        "</cliplist><cliplist frommodel='Unused' classid='other'><clip/></cliplist></animationdata>"));
    require(bind_actor_animation(defaults, "npc/fixture.adf").library == 0 &&
            defaults.libraries[0].clips[0].start == 0 && !defaults.libraries[0].clips[0].loops &&
            defaults.libraries[1].clips.empty() && !defaults.libraries[1].error.empty(),
            "source constructor defaults or unrelated bad-library isolation failed");
    Scene shared_geometry;
    shared_geometry.actor_tables = decode_actor_tables(encoded(
        "<classes><npc uid='16001' path='npc/shared.adf' meshid='Raw_Model' classid='016' biped='Raw_Biped'/>"
        "<npc uid='17085' path='npc/shared.adf' meshid='Raw_Model' classid='017' biped='Raw_Biped'/>"
        "<npc uid='903' meshid='Raw_Model' classid='018'/>"
        "<npc uid='904' classid='018'/></classes>"), encoded(
        "<animationdata><cliplist frommodel='Raw_Model' classid='016'>"
        "<clip clipid='2' end='1' jumptoid='2' jumptoframe='0'/></cliplist>"
        "<cliplist frommodel='Raw_Model' classid='017'>"
        "<clip clipid='2' end='2' jumptoid='2' jumptoframe='0'/></cliplist>"
        "<cliplist frommodel='Raw_Model' classid='018'>"
        "<clip clipid='2' end='3' jumptoid='2' jumptoframe='0'/></cliplist>"
        "<cliplist frommodel='Raw_Model' classid='unselected'><clip/></cliplist></animationdata>"));
    shared_geometry.actor_tables.base = std::filesystem::weakly_canonical(".");
    shared_geometry.actor_tables.loaded = true;
    const auto selected_npc = [&](unsigned uid) -> const SourceActorRecord& {
        const auto& records = shared_geometry.actor_tables.source_records;
        const auto selected = std::find_if(records.begin(), records.end(),
            [uid](const SourceActorRecord& record) { return record.uid == uid; });
        require(records.size() == 4 && selected != records.end() &&
                selected->tag == "npc" && !selected->class_id.empty(),
                "selected NPC fixture UID, tag or class metadata lost");
        return *selected;
    };
    require(load_actor_animation(shared_geometry, ".", "npc/shared.adf").stopped &&
            load_actor_animation(shared_geometry, ".", "npc/shared.adf",
                selected_npc(16001)).library == 0 &&
            load_actor_animation(shared_geometry, ".", "npc/shared.adf",
                selected_npc(17085)).library == 1 &&
            load_actor_animation(shared_geometry, ".", "npc/shared.adf",
                selected_npc(903)).library == 2 &&
            load_actor_animation(shared_geometry, ".", "npc/shared.adf",
                selected_npc(904)).library == 2 &&
            !shared_geometry.actor_tables.libraries[3].error.empty(),
            "selected raw actor identity or template class failed to disambiguate shared geometry");
    auto wrong_binding = selected_npc(17085);
    wrong_binding.biped = "Other_Biped";
    require(load_actor_animation(shared_geometry, ".", "npc/shared.adf", wrong_binding).stopped,
            "selected actor tuple absent from classes.dat was accepted");
    auto unanimated = selected_npc(904);
    unanimated.class_id = "019"; // a classid/meshid pair without a cliplist binds no library and holds its pose (0045d1f0 miss)
    const auto held = load_actor_animation(shared_geometry, ".", "npc/shared.adf", unanimated);
    require(held.library == UINT32_MAX && !held.stopped, "classid without a cliplist did not bind an unanimated actor");
    for (const auto uid : {16002u, 903u}) {
        bool rejected = false;
        try { (void)source_actor_record(shared_geometry, uid); }
        catch (const std::exception&) { rejected = true; }
        require(rejected, "absent or wrong-tag selected source item UID was accepted");
    }
    const auto independent_ranges = decode_actor_tables(encoded(classes), encoded(
        "<animationdata><cliplist frommodel='Raw_Model' classid='016'>"
        "<clip clipid='2' jumptoid='2' jumptoframe='0'/>"
        "<clip clipid='3' start='30' end='60' loops='true' loopstart='0' loopend='90' jumptoid='2' jumptoframe='0'/>"
        "<clip clipid='4' start='30' end='60' loops='false' loopstart='120' loopend='90' jumptoid='2' jumptoframe='0'/>"
        "</cliplist></animationdata>"));
    require(bind_actor_animation(independent_ranges, "npc/fixture.adf").library == 0 &&
            independent_ranges.libraries[0].clips[0].start == 0 &&
            independent_ranges.libraries[0].clips[0].end == 0 &&
            !independent_ranges.libraries[0].clips[0].loops &&
            independent_ranges.libraries[0].clips[1].loop_start < independent_ranges.libraries[0].clips[1].start &&
            independent_ranges.libraries[0].clips[1].loop_end > independent_ranges.libraries[0].clips[1].end &&
            independent_ranges.libraries[0].clips[2].loop_start > independent_ranges.libraries[0].clips[2].loop_end &&
            !independent_ranges.libraries[0].clips[2].loops,
            "source zero-span constructor defaults or independently authored loop ranges rejected");
    auto indented_classes = classes;
    indented_classes.insert(indented_classes.find("<group>"), std::string(max_actor_string * 2, ' '));
    const auto indented_tables = decode_actor_tables(encoded(indented_classes), encoded(animation));
    require(bind_actor_animation(indented_tables, "npc/fixture.adf").library == 0,
            "table formatting whitespace incorrectly counted as a semantic string");
    const auto source_predicates = decode_actor_tables(encoded(classes), encoded(
        "<animationdata><cliplist frommodel='Raw_Model' classid='016'>"
        "<clip clipid=' 2 ' start=' 0 ' end='1 ' loops='False' jumptoid='2 ' jumptoframe=' 0 '/>"
        "<clip clipid='3' start='0' end='1' loops='TRUE' jumptoid='2' jumptoframe='0'/>"
        "<clip clipid='4' start='0' end='1' loopstart='0' loops='true' jumptoid='2' jumptoframe='0'/>"
        "<clip clipid='5' start='0' end='1' loops=' true' jumptoid='2' jumptoframe='0'/>"
        "</cliplist></animationdata>"));
    require(bind_actor_animation(source_predicates, "npc/fixture.adf").library == 0 &&
            source_predicates.libraries[0].clips[0].id == 2 &&
            source_predicates.libraries[0].clips[0].next_id == 2 &&
            !source_predicates.libraries[0].clips[0].loops &&
            !source_predicates.libraries[0].clips[1].loops &&
            source_predicates.libraries[0].clips[2].loops &&
            !source_predicates.libraries[0].clips[3].loops,
            "source numeric whitespace or exact true predicate changed");
    for (const auto& invalid_clip : {
        "<clip clipid='2' start='nan' end='1' loopstart='0' loops='false' jumptoid='0' jumptoframe='0'/>",
        "<clip clipid='2' start='2' end='1' loopstart='2' loops='false' jumptoid='0' jumptoframe='0'/>",
        "<clip clipid='2' start='0' end='1' loopstart='2' loops='true' jumptoid='0' jumptoframe='0'/>",
        "<clip clipid='2' clipid='3' start='0' end='1' loopstart='0' loops='false' jumptoid='0' jumptoframe='0'/>",
        "<clip clipid='4294967296' start='0' end='1' loopstart='0' loops='false' jumptoid='0' jumptoframe='0'/>",
        "<clip clipid='2' start='0' end='1' loops='true' jumptoid='0' jumptoframe='0'/>",
        "<clip clipid='2' start='0' end='1' loops='false' jumptoid='1x' jumptoframe='0'/>",
        "<clip clipid='2' start='0' end='1' loopstart='0' loops='false'/>"}) {
        bool rejected = false;
        try {
            const auto invalid_tables = decode_actor_tables(encoded(classes), encoded(
                std::string("<animationdata><cliplist frommodel='Raw_Model' classid='016'>") +
                invalid_clip + "</cliplist></animationdata>"));
            (void)bind_actor_animation(invalid_tables, "npc/fixture.adf");
        } catch (const std::exception&) { rejected = true; }
        require(rejected, "invalid actor clip schema accepted");
    }
    for (const auto& invalid : {std::string("<!DOCTYPE classes><classes/>"),
         std::string("<classes name='&custom;'/>"), std::string("<classes/>\0suffix", 17),
         std::string(max_table_file + 1, 'x'),
         std::string("<classes name='") + std::string(max_actor_string + 1, 'x') + "'/>"}) {
        bool rejected = false;
        try { (void)decode_actor_tables(encoded(invalid), encoded(animation)); }
        catch (const std::exception&) { rejected = true; }
        require(rejected, "unbounded or unsafe actor XML accepted");
    }
    std::string deep;
    for (unsigned i = 0; i < 17; ++i) deep += "<classes>";
    for (unsigned i = 0; i < 17; ++i) deep += "</classes>";
    bool depth_rejected = false;
    try { (void)decode_actor_tables(encoded(deep), encoded(animation)); }
    catch (const std::exception&) { depth_rejected = true; }
    require(depth_rejected, "actor XML depth limit ignored");
    std::string many_nodes = "<classes>";
    for (std::size_t i = 0; i < max_table_nodes; ++i) many_nodes += "<x/>";
    many_nodes += "</classes>";
    bool nodes_rejected = false;
    try { (void)decode_actor_tables(encoded(many_nodes), encoded(animation)); }
    catch (const std::exception&) { nodes_rejected = true; }
    require(nodes_rejected, "actor XML node limit ignored");
    std::string many_clips = "<animationdata><cliplist frommodel='Raw_Model' classid='016'>";
    for (unsigned i = 0; i < 4097; ++i)
        many_clips += "<clip clipid='" + std::to_string(i) +
            "' start='0' end='1' loopstart='0' loops='false' jumptoid='0' jumptoframe='0'/>";
    many_clips += "</cliplist></animationdata>";
    const auto clipped = decode_actor_tables(encoded(classes), encoded(many_clips));
    require(clipped.libraries[0].clips.empty() && !clipped.libraries[0].error.empty(),
            "actor per-library clip limit ignored");
    auto unmatched_case = tables;
    unmatched_case.libraries[0].model = "raw_model";
    require(bind_actor_animation(unmatched_case, "npc/fixture.adf").library == UINT32_MAX,
            "actor raw model key case was normalized"); // the hash key is case-sensitive: no cliplist, unanimated
    Scene cached;
    cached.actor_tables = tables;
    cached.actor_tables.base = std::filesystem::weakly_canonical(".");
    cached.actor_tables.loaded = true;
    require(load_actor_animation(cached, ".", "npc/fixture.adf").library == 0,
            "cached actor table binding failed");
    require(load_actor_animation(cached, ".", "npc/missing.adf").stopped && !cached.diagnostics.empty(),
            "missing actor binding did not stop with diagnostic");
    bool mixed_rejected = false;
    try { (void)load_actor_animation(cached, cached.actor_tables.base / "different-installation", "npc/fixture.adf"); }
    catch (const std::exception&) { mixed_rejected = true; }
    require(mixed_rejected, "mixed actor installation cache accepted");
}

namespace {
bool xml_character(std::uint32_t value) {
    return value == 9 || value == 10 || value == 13 ||
           (value >= 0x20 && value <= 0xd7ff) ||
           (value >= 0xe000 && value <= 0xfffd) ||
           (value >= 0x10000 && value <= 0x10ffff);
}

void validate_xml(std::string_view bytes, std::size_t max_depth, std::size_t max_nodes) {
    require(bytes.find('\0') == std::string_view::npos, "raw NUL in XML");
    std::size_t depth = 0, nodes = 0;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        const auto byte = static_cast<unsigned char>(bytes[i]);
        require(byte >= 0x20 || byte == 9 || byte == 10 || byte == 13, "invalid XML character");
        if (bytes.substr(i, 4) == "<!--") {
            const auto end = bytes.find("-->", i + 4);
            require(end != std::string_view::npos, "unterminated XML comment");
            i = end + 2;
        } else if (bytes[i] == '<') {
            require(i + 1 < bytes.size() && bytes[i + 1] != '!', "XML declarations/entities forbidden");
            if (bytes[i + 1] == '?') {
                const auto end = bytes.find("?>", i + 2);
                require(end != std::string_view::npos, "unterminated XML instruction");
                i = end + 1;
                continue;
            }
            const bool closing = bytes[i + 1] == '/';
            if (closing) { require(depth != 0, "unexpected XML close"); --depth; }
            else {
                ++depth;
                require(depth <= max_depth, "XML nesting limit exceeded");
                require(++nodes <= max_nodes, "XML node limit exceeded");
            }
            // The XML parser validates names; this scan bounds recursion without parsing twice.
            char quote = 0;
            std::size_t end = i + 1;
            for (; end < bytes.size(); ++end) {
                const char c = bytes[end];
                if (quote) { if (c == quote) quote = 0; }
                else if (c == '\'' || c == '"') quote = c;
                else if (c == '>') break;
            }
            require(end < bytes.size(), "unterminated XML element");
            if (!closing && end > i && bytes[end - 1] == '/') --depth;
            // Attribute entities are checked below as well as entities in element text.
        } else if (bytes[i] == '&') {
            const auto end = bytes.find(';', i + 1);
            require(end != std::string_view::npos, "unterminated XML entity");
            const auto entity = bytes.substr(i + 1, end - i - 1);
            if (entity.starts_with('#')) {
                const bool hex = entity.starts_with("#x");
                const auto digits = entity.substr(hex ? 2 : 1);
                std::uint32_t value = 0;
                const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), value, hex ? 16 : 10);
                require(!digits.empty() && parsed.ec == std::errc{} &&
                        parsed.ptr == digits.data() + digits.size() && xml_character(value), "invalid XML numeric entity");
            } else {
                require(entity == "amp" || entity == "apos" || entity == "quot" ||
                        entity == "lt" || entity == "gt", "XML custom entities forbidden");
            }
            i = end;
        }
    }
    require(depth == 0, "unterminated XML");
}

std::string turkish_utf8(std::string_view bytes) {
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const unsigned char byte : bytes) {
        std::uint32_t point = byte;
        switch (byte) {
        case 0xd0: point = 0x11e; break;
        case 0xdd: point = 0x130; break;
        case 0xde: point = 0x15e; break;
        case 0xf0: point = 0x11f; break;
        case 0xfd: point = 0x131; break;
        case 0xfe: point = 0x15f; break;
        default: break;
        }
        if (point < 0x80) result.push_back(static_cast<char>(point));
        else {
            result.push_back(static_cast<char>(0xc0 | (point >> 6)));
            result.push_back(static_cast<char>(0x80 | (point & 0x3f)));
        }
    }
    return result;
}

void validate_attributes(const Tree& node, std::initializer_list<std::string_view> allowed) {
    require(std::all_of(node.data().begin(), node.data().end(), [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    }), "unexpected help XML text content");
    const auto attributes = node.get_child_optional("<xmlattr>");
    if (!attributes) return;
    for (auto item = attributes->begin(); item != attributes->end(); ++item) {
        require(std::find(allowed.begin(), allowed.end(), item->first) != allowed.end(), "unknown help XML attribute");
        require(std::none_of(attributes->begin(), item, [&](const auto& previous) {
            return previous.first == item->first;
        }), "duplicate help XML attribute");
    }
}

std::string take_text(Tree& node, const char* key, std::size_t& text_bytes) {
    auto& value = node.get_child("<xmlattr>").get_child(key).data();
    require(value.size() <= max_text - text_bytes, "help text limit exceeded");
    text_bytes += value.size();
    return std::move(value);
}

Tree parse_xml(std::string_view bytes, std::size_t max_depth, std::size_t max_nodes, bool trim_text = false) {
    validate_xml(bytes, max_depth, max_nodes);
    auto utf8 = turkish_utf8(bytes);
    utf8.push_back('\0');
    namespace rx = boost::property_tree::detail::rapidxml;
    rx::xml_document<char> document;
    // Boost read_xml uses parse<0>; use its same parser/converter with closing-name validation.
    if (trim_text)
        document.parse<rx::parse_validate_closing_tags | rx::parse_trim_whitespace>(utf8.data());
    else
        document.parse<rx::parse_validate_closing_tags>(utf8.data());
    Tree tree;
    for (auto* node = document.first_node(); node; node = node->next_sibling())
        boost::property_tree::xml_parser::read_xml_node(node, tree, boost::property_tree::xml_parser::no_comments);
    return tree;
}

std::vector<HelpPage> decode_help(std::string_view bytes) {
    require(bytes.size() <= max_file, "help XML file limit exceeded");
    auto tree = parse_xml(bytes, 4, max_table_nodes);
    require(tree.size() == 1 && tree.begin()->first == "Library", "expected one help Library root");
    auto& library = tree.begin()->second;
    validate_attributes(library, {});
    std::size_t text_bytes = 0, paragraph_count = 0;
    std::vector<HelpPage> pages;
    for (auto& book : library) {
        require(book.first == "Book", "expected help Book");
        validate_attributes(book.second, {"name"});
        const auto book_name = take_text(book.second, "name", text_bytes);
        for (auto& page : book.second) {
            if (page.first == "<xmlattr>") continue;
            require(page.first == "Page" && pages.size() < max_pages, "invalid help Page or page limit exceeded");
            validate_attributes(page.second, {"header"});
            HelpPage output{book_name, take_text(page.second, "header", text_bytes), {}};
            // The returned model repeats the book name per page; include those copies in its text budget.
            require(book_name.size() <= max_text - text_bytes, "help text limit exceeded");
            text_bytes += book_name.size();
            for (auto& paragraph : page.second) {
                if (paragraph.first == "<xmlattr>") continue;
                require(paragraph.first == "Paragraph" && paragraph_count < max_paragraphs,
                        "invalid help Paragraph or paragraph limit exceeded");
                ++paragraph_count;
                validate_attributes(paragraph.second, {"text", "color"});
                require(paragraph.second.size() == 1, "help Paragraph cannot have child elements");
                HelpParagraph value{take_text(paragraph.second, "text", text_bytes)};
                if (const auto color_node = paragraph.second.get_child_optional("<xmlattr>.color")) {
                    const auto& color = color_node->data();
                    const auto parsed = std::from_chars(color.data(), color.data() + color.size(), value.color, 10);
                    require(!color.empty() && parsed.ec == std::errc{} && parsed.ptr == color.data() + color.size(),
                            "invalid help Paragraph color");
                }
                output.paragraphs.push_back(std::move(value));
            }
            pages.push_back(std::move(output));
        }
    }
    return pages;
}

std::string actor_path(std::string_view path, bool include_objects = false) {
    require(!path.empty() && path.size() <= max_actor_string, "actor member path missing or too long");
    std::string result;
    if (include_objects) result.reserve(path.size() + 8);
    result.assign(path);
    for (auto& c : result) {
        require(static_cast<unsigned char>(c) >= 0x20, "actor member control character forbidden");
        if (c == '\\') c = '/';
        else if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    require(result.front() != '/' && result.find(':') == std::string::npos,
            "absolute actor member path forbidden");
    if (result.starts_with("objects/")) result.erase(0, 8);
    require(!result.empty(), "actor member path missing after objects prefix");
    for (std::size_t first = 0; first < result.size();) {
        const auto last = result.find('/', first);
        const auto part = std::string_view(result).substr(first, last == std::string::npos ? last : last - first);
        require(!part.empty() && part != "." && part != "..", "actor member traversal or empty component forbidden");
        if (last == std::string::npos) break;
        first = last + 1;
        require(first < result.size(), "actor member trailing separator forbidden");
    }
    if (include_objects) result.insert(0, "objects/");
    return result;
}

void bound_actor_tree(const Tree& tree, std::size_t& nodes) {
    require(++nodes <= max_table_nodes, "actor table XML node/attribute limit exceeded");
    require(tree.data().size() <= max_actor_string, "actor table XML string limit exceeded");
    for (const auto& item : tree) {
        require(item.first.size() <= max_actor_string, "actor table XML name limit exceeded");
        bound_actor_tree(item.second, nodes);
    }
}

void validate_actor_attributes(const Tree& node) {
    const auto attributes = node.get_child_optional("<xmlattr>");
    if (!attributes) return;
    for (auto item = attributes->begin(); item != attributes->end(); ++item)
        require(std::none_of(attributes->begin(), item, [&](const auto& previous) {
            return previous.first == item->first;
        }), "duplicate actor metadata attribute");
}

const std::string* actor_attribute(const Tree& node, const char* key) {
    const auto attributes = node.get_child_optional("<xmlattr>");
    if (!attributes) return nullptr;
    const auto attribute = attributes->find(key);
    return attribute == attributes->not_found() ? nullptr : &attribute->second.data();
}

const std::string& actor_required(const Tree& node, const char* key) {
    const auto* value = actor_attribute(node, key);
    if (!value || value->empty()) throw std::runtime_error(std::string("missing actor metadata attribute: ") + key);
    return *value;
}

template<class T> T actor_number(const std::string& text, const char* key) {
    // Source scalar conversion accepts ASCII whitespace; tuple keys remain raw.
    std::string_view digits(text);
    const auto whitespace = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
    };
    while (!digits.empty() && whitespace(digits.front())) digits.remove_prefix(1);
    while (!digits.empty() && whitespace(digits.back())) digits.remove_suffix(1);
    T value{};
    const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), value);
    if (digits.empty() || parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size())
        throw std::runtime_error(std::string("invalid actor numeric attribute: ") + key);
    if constexpr (std::is_floating_point_v<T>)
        if (!std::isfinite(value)) throw std::runtime_error(std::string("nonfinite actor attribute: ") + key);
    return value;
}

ActorClip decode_actor_clip(const Tree& node) {
    validate_actor_attributes(node);
    require(node.size() == 1 && node.begin()->first == "<xmlattr>", "actor clip cannot have child elements");
    ActorClip clip;
    clip.id = actor_number<std::uint32_t>(actor_required(node, "clipid"), "clipid");
    if (const auto* name = actor_attribute(node, "name")) clip.name = *name;
    constexpr float frame_seconds = 0.033333335f; // Original 00594210, float bits 3d088889.
    // 0045e640 initializes start/end/loops to zero; 0045dba0 substitutes
    // end for absent loopend. The constructor does not initialize loopstart or jump fields.
    if (const auto* start = actor_attribute(node, "start"))
        clip.start = actor_number<float>(*start, "start") * frame_seconds;
    if (const auto* end = actor_attribute(node, "end"))
        clip.end = actor_number<float>(*end, "end") * frame_seconds;
    // Original strcmp(value, "true") enables looping only for this exact spelling.
    if (const auto* loops = actor_attribute(node, "loops")) clip.loops = *loops == "true";
    const auto* loop_start = actor_attribute(node, "loopstart");
    require(!clip.loops || loop_start, "missing active actor loopstart (source constructor leaves it unresolved)");
    // Zero here is an inactive local representation, not an original default loopstart.
    if (loop_start) clip.loop_start = actor_number<float>(*loop_start, "loopstart") * frame_seconds;
    const auto* loop_end = actor_attribute(node, "loopend");
    clip.loop_end = loop_end ? actor_number<float>(*loop_end, "loopend") * frame_seconds : clip.end;
    clip.next_id = actor_number<std::uint32_t>(actor_required(node, "jumptoid"), "jumptoid");
    clip.next_frame = actor_number<std::uint32_t>(actor_required(node, "jumptoframe"), "jumptoframe");
    require(clip.start >= 0 && clip.end >= clip.start && std::isfinite(clip.end - clip.start) &&
            clip.loop_start >= 0 && clip.loop_end >= 0 &&
            (!clip.loops || clip.loop_end >= clip.loop_start),
            "invalid actor clip or loop interval");
    // 0045dba0 loads audio, audio2, audio3 into one per-clip sample list (0045c890 picks rand()%n).
    for (const char* key : {"audio", "audio2", "audio3"})
        if (const auto* audio = actor_attribute(node, key)) clip.audio.push_back(*audio);
    if (const auto* effect = actor_attribute(node, "visual_effect")) clip.visual_effect = *effect;
    // state: action class per clip (names: 1 run, 2 walk, 3 wait, 4 combat stance, 5 attack, 6 dodge, 7 damage, 8 death);
    // the native client selects monster combat clips by it, and NPC walk vs run by forwardspeed (0045dba0 only stores it).
    if (const auto* state = actor_attribute(node, "state")) clip.state = actor_number<std::uint32_t>(*state, "state");
    if (const auto* speed = actor_attribute(node, "forwardspeed")) clip.forward_speed = actor_number<float>(*speed, "forwardspeed");
    return clip;
}

Tree parse_source_table(std::string& bytes) {
    require(bytes.size() <= max_table_file, "source table 4 MiB file limit exceeded");
    for (std::size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<char>(static_cast<unsigned char>(bytes[i]) ^ ((i * 0x57 + 0x55) & 255) ^ 0xaa);
    // Trim formatting text only in tables so root indentation cannot become one
    // concatenated semantic string; help attribute/text behavior is unchanged.
    auto tree = parse_xml(bytes, 16, max_table_nodes, true);
    std::size_t nodes = 0;
    bound_actor_tree(tree, nodes);
    return tree;
}

std::string read_source_table(const std::filesystem::path& base, const char* name) {
    const auto path = base / "data" / name;
    const auto size = std::filesystem::file_size(path);
    require(size <= max_table_file, "source table 4 MiB file limit exceeded");
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("cannot open source table: " + path.string());
    std::string bytes(static_cast<std::size_t>(size), '\0');
    stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (stream.gcount() != static_cast<std::streamsize>(bytes.size()) ||
        stream.peek() != std::char_traits<char>::eof())
        throw std::runtime_error("source table changed or could not be read completely: " + path.string());
    return bytes;
}

std::vector<SourceSkill> decode_source_skills(std::string bytes) {
    const auto tree = parse_source_table(bytes);
    require(tree.size() == 1 && tree.begin()->first == "skills", "expected one skills root");
    const auto validate = [&](const auto& visit, const Tree& node) -> void {
        validate_actor_attributes(node);
        for (const auto& child : node)
            if (child.first != "<xmlattr>") visit(visit, child.second);
    };
    validate(validate, tree.begin()->second);
    std::array<bool, 3 * 30> identities{};
    std::vector<SourceSkill> skills;
    skills.reserve(std::min(tree.begin()->second.size(), identities.size()));
    for (const auto& child : tree.begin()->second) {
        if (child.first != "skill") continue;
        SourceSkill skill;
        skill.classindex = actor_number<int>(actor_required(child.second, "classindex"), "classindex");
        skill.skillno = actor_number<int>(actor_required(child.second, "skillno"), "skillno");
        require(skill.classindex >= 0 && skill.classindex < 3 && skill.skillno >= 0 && skill.skillno < 30,
                "source skill identity outside 3 by 30 table");
        const auto identity = static_cast<std::size_t>(skill.classindex * 30 + skill.skillno);
        require(!identities[identity], "duplicate source skill identity");
        identities[identity] = true;
        if (const auto* hasicon = actor_attribute(child.second, "hasicon"))
            skill.hasicon = actor_number<int>(*hasicon, "hasicon") != 0;
        if (const auto* icon = actor_attribute(child.second, "icon"))
            skill.icon = actor_number<int>(*icon, "icon");
        if (const auto* sound = actor_attribute(child.second, "sound")) skill.sound = *sound;
        if (const auto* sound = actor_attribute(child.second, "femalesound")) skill.female_sound = *sound;
        if (const auto* name = actor_attribute(child.second, "name")) skill.name = *name;
        if (const auto* detail = actor_attribute(child.second, "detail")) skill.detail = *detail; // '|' line breaks, "%%" literal percent
        // Original skill 15 is a singleton; retain raw identities, not ranks or server actions.
        skills.push_back(skill);
    }
    return skills;
}

ActorTables decode_actor_tables(std::string classes, std::string animation) {
    auto class_tree = parse_source_table(classes);
    auto animation_tree = parse_source_table(animation);
    require(class_tree.size() == 1 && class_tree.begin()->first == "classes", "expected one classes root");
    require(animation_tree.size() == 1 && animation_tree.begin()->first == "animationdata",
            "expected one animationdata root");
    ActorTables tables;
    const auto collect_bindings = [&](const auto& visit, const Tree& node, const std::string& tag) -> void {
        if (const auto* uid = actor_attribute(node, "uid")) {
            SourceActorRecord record;record.uid=actor_number<unsigned>(*uid,"uid");record.tag=tag;
            const auto copy=[&](const char* key,std::string& field){if(const auto* value=actor_attribute(node,key))field=*value;};
            copy("path",record.path);copy("meshid",record.model);copy("classid",record.class_id);copy("biped",record.biped);
            copy("objecttype",record.object_type);copy("bodypiecelist",record.bodypieces);copy("suitableslot",record.suitable_slot);copy("shadowtype",record.shadow);copy("diffuse_texture",record.diffuse);
            copy("icon", record.icon_word); copy("pose", record.pose_word);
            const auto bound=[&](const char* key,float& field){if(const auto* value=actor_attribute(node,key))field=std::strtof(value->c_str(),nullptr);}; // 004637b0 atof
            bound("boundx",record.bound_x);bound("boundy",record.bound_y);bound("boundz",record.bound_z);
            try {
                if (const auto* icon = actor_attribute(node, "icon"))
                    record.icon = actor_number<int>(*icon, "icon");
                if (const auto* pose = actor_attribute(node, "pose"))
                    record.pose = actor_number<int>(*pose, "pose");
            } catch (const std::exception& error) {
                record.icon = -1;
                record.pose = 0;
                record.error = error.what();
            }
            if(!record.path.empty()){try{record.path=actor_path(record.path);}catch(const std::exception&){record.path.clear();}}
            tables.source_records.push_back(std::move(record));
        }
        if (const auto* path = actor_attribute(node, "path")) {
            validate_actor_attributes(node);
            ActorBinding binding;
            // Non-actor equipment/sentinel records can lack actor fields or valid member paths.
            try { binding.path = actor_path(*path); } catch (const std::exception&) { binding.path.clear(); }
            if (!binding.path.empty()) {
                if (const auto* model = actor_attribute(node, "meshid")) binding.model = *model;
                if (const auto* class_id = actor_attribute(node, "classid")) binding.class_id = *class_id;
                if (const auto* biped = actor_attribute(node, "biped")) binding.biped = *biped;
                const auto identical = [&](const ActorBinding& previous) {
                    return previous.path == binding.path && previous.model == binding.model &&
                           previous.class_id == binding.class_id && previous.biped == binding.biped;
                };
                if (std::none_of(tables.bindings.begin(), tables.bindings.end(), identical))
                    tables.bindings.push_back(std::move(binding));
            }
        }
        for (const auto& child : node)
            if (child.first != "<xmlattr>") visit(visit, child.second, child.first);
    };
    collect_bindings(collect_bindings, class_tree.begin()->second, "classes");
    for (const auto& item : animation_tree.begin()->second) {
        if (item.first == "<xmlattr>") continue;
        require(item.first == "cliplist", "expected actor cliplist element");
        ActorLibrary library;
        if (const auto* model = actor_attribute(item.second, "frommodel")) library.model = *model;
        if (const auto* class_id = actor_attribute(item.second, "classid")) library.class_id = *class_id;
        try {
            validate_actor_attributes(item.second);
            require(!library.model.empty() && !library.class_id.empty(), "actor library missing frommodel/classid");
            for (const auto& child : item.second) {
                if (child.first == "<xmlattr>") continue;
                require(child.first == "clip" && library.clips.size() < 4096,
                        "invalid actor clip element or per-library 4096 clip limit exceeded");
                auto clip = decode_actor_clip(child.second);
                require(std::none_of(library.clips.begin(), library.clips.end(), [&](const ActorClip& previous) {
                    return previous.id == clip.id;
                }), "duplicate actor clip ID");
                library.clips.push_back(std::move(clip));
            }
            require(!library.clips.empty(), "actor library has no clips");
        } catch (const std::exception& error) {
            library.clips.clear();
            library.error = error.what();
        }
        tables.libraries.push_back(std::move(library));
    }
    return tables;
}

ActorAnimation bind_actor_animation(const ActorTables& tables, const std::string& member,
                                   const SourceActorRecord* selected) {
    const auto path = actor_path(member);
    const bool template_record = selected && selected->path.empty();
    if (selected) {
        if (!selected->error.empty())
            throw std::runtime_error("invalid selected classes.dat metadata: " + selected->error);
        require(template_record || actor_path(selected->path) == path,
                "selected classes.dat record does not reference actor member");
    }
    const ActorBinding* binding = nullptr;
    for (const auto& candidate : tables.bindings) {
        if (candidate.path != path) continue;
        if (!selected) {
            require(!binding, "ambiguous classes.dat actor path binding (distinct model/classid/biped tuples)");
        } else if (!template_record) {
            if (candidate.model != selected->model || candidate.class_id != selected->class_id ||
                candidate.biped != selected->biped) continue;
        } else {
            if (candidate.model.empty() || (!selected->model.empty() && candidate.model != selected->model))
                continue;
            require(!binding || binding->model == candidate.model,
                    "ambiguous classes.dat geometry model for selected NPC template");
        }
        binding = &candidate;
    }
    require(binding, "classes.dat has no actor binding for selected member");
    const auto& model = selected && !selected->model.empty() ? selected->model : binding->model;
    const auto& class_id = selected ? selected->class_id : binding->class_id;
    require(!model.empty() && !class_id.empty(), "selected classes.dat record missing meshid/classid");
    std::uint32_t library_index = UINT32_MAX;
    for (std::size_t i = 0; i < tables.libraries.size(); ++i) {
        const auto& library = tables.libraries[i];
        if (library.model != model || library.class_id != class_id) continue;
        require(library_index == UINT32_MAX, "ambiguous animation.dat frommodel/classid library");
        library_index = static_cast<std::uint32_t>(i);
    }
    // 0045dba0 keys each cliplist by hash("%s%s" of its classid and frommodel) and 0045d1f0 looks clips up by that key, so
    // a record whose classid/meshid pair has no cliplist (Karaköy sığınak kapağı 132069: classid 737 belongs to
    // ID_KKOY_SIG_KAPI) gets no clip for any action and 0047bea0 holds its initial pose: it still renders, unanimated.
    if (library_index == UINT32_MAX) return {};
    const auto& library = tables.libraries[library_index];
    if (!library.error.empty()) throw std::runtime_error("invalid selected animation.dat library: " + library.error);
    // No clip 2 is not a binding failure: 0047bea0 resolves an absent action to no clip and holds the pose
    // (pigeons, flags, doors start with their state-3 wait clip only).
    require(!library.clips.empty(), "selected animation.dat library has no clips");
    ActorAnimation actor;
    actor.library = library_index;
    return actor;
}
} // namespace

std::vector<HelpPage> load_help(const std::filesystem::path& base) {
    const auto path = base / "helps.xml";
    std::error_code error;
    const auto exists = std::filesystem::exists(path, error);
    if (error) throw std::runtime_error("cannot inspect helps.xml: " + error.message());
    if (!exists) return {};
    const auto size = std::filesystem::file_size(path);
    require(size <= max_file, "help XML file limit exceeded");
    std::ifstream stream(path, std::ios::binary);
    require(static_cast<bool>(stream), "cannot open helps.xml");
    std::string bytes(static_cast<std::size_t>(size), '\0');
    stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    require(stream.gcount() == static_cast<std::streamsize>(bytes.size()) && stream.peek() == std::char_traits<char>::eof(),
            "helps.xml changed or could not be read completely");
    return decode_help(bytes);
}

std::vector<SourceSkill> load_source_skills(const std::filesystem::path& base) {
    for (const auto& candidate : {base, base / "base", base / "app" / "base"}) {
        if (std::filesystem::exists(candidate / "objects.dat") ||
            std::filesystem::exists(candidate / "data" / "skills.dat"))
            return decode_source_skills(read_source_table(candidate, "skills.dat"));
    }
    throw std::runtime_error("source installation has no data/skills.dat: " + base.string());
}

std::vector<std::string> load_source_playlist(const std::filesystem::path& base, const char* name) {
    // 004596f0 loads data\playlist.dat / data\playlistsound.dat in file order; 004573e0 indexes
    // that order with the music/sound grid byte (name attributes are labels, "nothing" is silent).
    auto bytes = read_source_table(base, name);
    const auto tree = parse_source_table(bytes);
    require(tree.size() == 1 && tree.begin()->first == "playlist", "expected one playlist root");
    std::vector<std::string> songs;
    for (const auto& child : tree.begin()->second)
        if (child.first == "song") songs.push_back(child.second.data());
    return songs;
}

std::vector<SourceRecord> load_source_records(const std::filesystem::path& base, const char* name) {
    auto bytes = read_source_table(base, name);
    const auto tree = parse_source_table(bytes);
    require(tree.size() == 1, "expected one source table root");
    // parse_source_table already bounds depth/node count, so plain recursion is safe.
    const auto collect = [](const auto& self, const Tree& node) -> std::vector<SourceRecord> {
        std::vector<SourceRecord> records;
        for (const auto& child : node) {
            if (child.first == "<xmlattr>" || child.first == "<xmlcomment>") continue;
            SourceRecord record{child.first, {}, self(self, child.second)};
            if (const auto attributes = child.second.get_child_optional("<xmlattr>"))
                for (const auto& attribute : *attributes) record.attributes.emplace(attribute.first, attribute.second.data());
            records.push_back(std::move(record));
        }
        return records;
    };
    return collect(collect, tree.begin()->second);
}

namespace {
ActorAnimation load_actor_animation_impl(Scene& scene, const std::filesystem::path& base,
                                        const std::string& member, const SourceActorRecord* selected) {
    const auto normalized_base = std::filesystem::weakly_canonical(base);
    if (scene.actor_tables.loaded && scene.actor_tables.base != normalized_base)
        throw std::runtime_error("cannot mix actor source installations in one Scene");
    if (!scene.actor_tables.loaded) {
        ActorTables tables;
        try {
            tables = decode_actor_tables(read_source_table(normalized_base, "classes.dat"),
                                         read_source_table(normalized_base, "animation.dat"));
        } catch (const std::exception& error) {
            scene.diagnostics.push_back("Actor table load failed; attached actors retain source initial pose: " +
                                        std::string(error.what()));
        }
        tables.base = normalized_base;
        tables.loaded = true;
        scene.actor_tables = std::move(tables);
    }
    try {
        return bind_actor_animation(scene.actor_tables, member, selected);
    } catch (const std::exception& error) {
        scene.diagnostics.push_back("Actor binding " + member +
            (selected ? " UID " + std::to_string(selected->uid) : "") +
            " retains source initial pose: " + error.what());
        ActorAnimation actor;
        actor.stopped = true;
        return actor;
    }
}
} // namespace

ActorAnimation load_actor_animation(Scene& scene, const std::filesystem::path& base, const std::string& member) {
    return load_actor_animation_impl(scene, base, member, nullptr);
}

ActorAnimation load_actor_animation(Scene& scene, const std::filesystem::path& base, const std::string& member,
                                   const SourceActorRecord& selected) {
    return load_actor_animation_impl(scene, base, member, &selected);
}

std::string source_object_member(const std::string& path) {
    return actor_path(path, true);
}
} // namespace ikv
