#include "ikv/world.hpp"
#include "ikv/scene.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <map>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
ikv::Json record(int id, int x = 0) {
    return {{"id", id}, {"account_id", id}, {"name", "Player" + std::to_string(id)},
            {"class_id", 1}, {"appearance", ikv::Json::object()}, {"state", {{"x", x}, {"y", 0}}}};
}
void ticks(ikv::World& world, int count) { while (count-- > 0) world.advance(); }
bool event(ikv::Json value, const char* type, const char* code = "") {
    for (const auto& e : value.at("events"))
        if (e.at("type") == type && (std::string(code).empty() || e.value("code", "") == code)) return true;
    return false;
}
std::string uid(const ikv::Json& state, const char* item) {
    for (const auto& entry : state.at("character").at("state").at("inventory"))
        if (entry.at("item") == item) return entry.at("uid");
    throw std::runtime_error("missing item");
}
bool holds(const ikv::Json& state, const char* item) {
    for (const auto& entry : state.at("character").at("state").at("inventory")) if (entry.at("item") == item) return true;
    return false;
}
ikv::Json terms(const ikv::Json& state) {
    ikv::Json result;
    for(const auto& value:state.at("events"))if(value.at("type")=="trade")result=value;
    check(!result.is_null(),"authoritative trade terms missing");return result;
}
// A frame as the client sees it: the Type::catalog parts (recipe_book/bank_view) folded in, as client_session.cpp does.
ikv::Json folded(ikv::World& world, uint32_t id) {
    auto frame=world.state(id); const auto bank=world.bank_view(id);
    frame["catalogs"]["recipes"]=world.recipe_book(id).at("recipes");
    if(!bank.at("bank").is_null())frame["bank"]=bank.at("bank");
    return frame;
}
ikv::Json independent_mechanics(ikv::Json config, const ikv::Json& fixture) {
    // Independently authored narrow mechanics content (tests/independent_content.json): no original support assets required.
    config["provenance"] = "NONCANONICAL independently authored mechanics test fixture";
    for(const char* key:{"zones","defaults","items","quests","npcs","activities"})config[key]=fixture.at(key);
    config["recipes"]=ikv::Json::object(); // original recipes reference generated items; economy_mechanics() adds its own
    // Assertions below use this compact mechanics scale, not the source-scaled playable distances.
    for(const auto& [key,value]:std::initializer_list<std::pair<const char*,int>>{{"movement_step",100},{"cell_size",1000},{"aoi",2000},{"interaction_range",150},{"combat_range",200},{"trade_range",200},{"death_resurrect_delay",20}})config[key]=value;
    for(auto& [id,skill]:config["skills"].items()){if(skill.contains("range"))skill["range"]=skill.at("source")[0]==0?200:700;if(skill.contains("radius"))skill["radius"]=300;}
    return config;
}
const ikv::Json& entity(const ikv::Json& state, uint32_t id);
// Üretim/Toplama/Banka with independent fixture items (not original): authority, persistence atomicity and dupe-proofing.
void economy_mechanics(ikv::Json config) {
    const std::string ore_uid(32,'1'),scroll_uid(32,'2');
    for(const auto& [id,name]:std::initializer_list<std::pair<const char*,const char*>>{{"test_ore","Test Cevheri"},{"test_rare","Saf Test Cevheri"},{"test_ingot","Test Külçe"},{"test_scroll","Test Reçetesi"}})
        config["items"][id]={{"name",name},{"stack",20},{"price",5},{"level",1}};
    config["recipes"]={{"test_scroll",{{"craft","madenci"},{"level",1},{"product","test_ingot"},{"quantity",1},{"components",{{{"item","test_ore"},{"quantity",2}}}}}}};
    config["economy"]["gather_ticks"]=3;config["economy"]["gather_rare_percent"]=100;config["economy"]["bank_slots"]=2;config["quests"]=ikv::Json::object(); // rare roll forced; 2 cells per Banka tab
    const uint32_t node=2147493700u,banker=2147493701u;
    config["npcs"]={{{"id",node},{"kind","node"},{"name","Test Cevheri"},{"zone","eminonu"},{"x",100},{"y",0},{"hp",100},{"dialogue","Maden"},{"quests",ikv::Json::array()},{"shop",ikv::Json::array()},
                     {"gather",{{"craft","madenci"},{"level",1},{"item","test_ore"},{"rare","test_rare"},{"respawn",40}}}},
                    {{"id",banker},{"kind","questgiver"},{"banker",true},{"name","Bankacı"},{"zone","eminonu"},{"x",0},{"y",100},{"hp",100},{"dialogue","Kasa"},{"quests",ikv::Json::array()},{"shop",ikv::Json::array()},
                     {"equipment",{{{"slot",11},{"source_uid",13226}}}}}}; // classes.dat rhweaponid (Bab-ı Ali Muhafızı: Tüfek)
    std::vector<ikv::CharacterSave> saved;bool fail=false;
    ikv::World world(config,[&](std::span<const ikv::CharacterSave> states){if(fail)throw std::runtime_error("injected persistence failure");saved.assign(states.begin(),states.end());});
    auto player=record(1);player["state"]["level"]=2;player["state"]["crafts"]={{"madenci",1}};
    player["state"]["inventory"]={{{"uid",ore_uid},{"item","test_ore"},{"quantity",2}},{{"uid",scroll_uid},{"item","test_scroll"},{"quantity",1}}};
    world.add(1,1,1,player);world.add(2,2,2,record(2));
    check(entity(world.state(1),banker).at("equipment")==ikv::Json::array({{{"slot",11},{"source_uid",13226}}}),"an NPC's held weapon reaches the client like a player's visible equipment");
    const auto own=[&](){return world.state(1).at("character").at("state");};
    const auto count=[&](const ikv::Json& items,const char* item){int n=0;for(const auto& e:items)if(e.at("item")==item)n+=e.at("quantity").get<int>();return n;};
    const auto phase=[&](const ikv::Json& frame,const char* name){for(const auto& e:frame.at("events"))if(e.at("type")=="gather" && e.at("phase")==name)return true;return false;};
    // Toplama: profession gate (VOD wording), timed gather, shared exhaustion, regrowth, cancel on movement.
    world.action(2,{{"action","gather"},{"target",node}});
    bool gated=false;const auto denied=world.state(2);for(const auto& e:denied.at("events"))gated=gated||(e.at("type")=="error" && e.at("message")=="Buradan toplayabilmek için 1 seviye Madenci olmanız gerekir");
    check(gated,"gathering requires the node's profession level");
    world.action(1,{{"action","gather"},{"target",node}});
    check(phase(world.state(2),"start") && count(own().at("inventory"),"test_ore")==2,"gather start is projected; nothing gained yet");
    // The client builds "Toplandı: <ad>" before applying the frame carrying the event, so the previous frame's catalog must already
    // name every gained item, including a rare it has never owned.
    ticks(world,2);const auto before=world.state(1);
    check(!holds(before,"test_rare") && before.at("catalogs").at("items").contains("test_rare") && before.at("catalogs").at("items").contains("test_ore"),
          "a visible node's ordinary and rare gather items are catalogued before the player owns them");
    ticks(world,1);
    {const auto done=world.state(1);ikv::Json gained;for(const auto& e:done.at("events"))if(e.at("type")=="gather" && e.at("phase")=="done")gained=e.at("items");
     check(gained==ikv::Json::array({"test_ore","test_rare"}),"a forced rare roll yields the ordinary and rare items");
     for(const auto& item:gained)check(before.at("catalogs").at("items").contains(item) && done.at("catalogs").at("items").contains(item),"every gathered item resolves in the previous and current catalogs");}
    check(phase(world.state(2),"done") && count(own().at("inventory"),"test_ore")==3 && count(own().at("inventory"),"test_rare")==1 && !entity(world.state(1),node).at("alive").get<bool>(),"gather completes after gather_ticks and exhausts the node");
    world.action(1,{{"action","gather"},{"target",node}});check(event(world.state(1),"error","exhausted"),"exhausted node refuses gathering");
    ticks(world,40);world.action(1,{{"action","gather"},{"target",node}});world.input(1,1,0,1);ticks(world,4);
    check(count(own().at("inventory"),"test_ore")==3 && phase(world.state(2),"cancel"),"moving cancels gathering");
    world.input(1,0,0,2);world.destination(1,0,0,3);ticks(world,8);
    // Üretim: learn by using the scroll, gate on learned recipe and inputs, consume exactly, forget.
    world.action(1,{{"action","craft"},{"recipe","test_scroll"}});check(event(world.state(1),"error","recipe"),"unlearned recipe cannot be crafted");
    world.action(1,{{"action","use"},{"uid",scroll_uid}});
    check(own().at("recipes")==ikv::Json::array({"test_scroll"}) && count(own().at("inventory"),"test_scroll")==0 &&
          folded(world,1).at("catalogs").at("recipes").contains("test_scroll"),"using a recipe scroll memorises it");
    world.action(1,{{"action","craft"},{"recipe","test_scroll"}});
    {const auto crafted=world.state(1);const auto& bag=crafted.at("character").at("state").at("inventory");
     check(count(bag,"test_ingot")==1 && count(bag,"test_ore")==1 && event(crafted,"craft"),"craft consumes inputs and yields product");}
    world.action(1,{{"action","craft"},{"recipe","test_scroll"}});check(event(world.state(1),"error","components") && count(own().at("inventory"),"test_ingot")==1,"missing inputs refuse crafting");
    // Banka: session gate, then deposit/withdraw/currency are atomic with state; duplicates and failed commits never duplicate.
    std::string ingot;{const auto bag=own();for(const auto& e:bag.at("inventory"))if(e.at("item")=="test_ingot")ingot=e.at("uid");}
    world.action(1,{{"action","bank_deposit"},{"uid",ingot},{"quantity",1}});check(event(world.state(1),"error","bank"),"bank requires a banker session");
    world.action(1,{{"action","bank_open"},{"target",banker}});
    world.action(1,{{"action","bank_deposit"},{"uid",ingot},{"quantity",1}});
    world.action(1,{{"action","bank_deposit"},{"uid",ingot},{"quantity",1}}); // duplicate request in the same tick
    auto frame=folded(world,1);
    check(event(frame,"error","ownership") && frame.at("bank").at("items").size()==1 && count(frame.at("bank").at("items"),"test_ingot")==1 &&
          count(frame.at("character").at("state").at("inventory"),"test_ingot")==0 && saved.back().bank==ikv::Json({{"items",frame.at("bank").at("items")},{"currency",0}}),
          "duplicate deposit rejected; bank and state committed together");
    const auto banked=frame.at("bank").at("items")[0].at("uid").get<std::string>();
    fail=true;world.action(1,{{"action","bank_withdraw"},{"uid",banked},{"quantity",1}});fail=false;
    frame=folded(world,1);
    check(event(frame,"error","persistence") && count(frame.at("bank").at("items"),"test_ingot")==1 && count(frame.at("character").at("state").at("inventory"),"test_ingot")==0,
          "failed withdraw commit leaves both bank and bag unchanged");
    world.action(1,{{"action","bank_withdraw"},{"uid",banked},{"quantity",1}});world.action(1,{{"action","bank_withdraw"},{"uid",banked},{"quantity",1}});
    frame=folded(world,1);
    check(event(frame,"error","ownership") && frame.at("bank").at("items").empty() && count(frame.at("character").at("state").at("inventory"),"test_ingot")==1,
          "duplicate withdraw rejected; exactly one ingot exists");
    world.action(1,{{"action","bank_currency"},{"amount",60}});world.action(1,{{"action","bank_currency"},{"amount",-61}});
    fail=true;world.action(1,{{"action","bank_currency"},{"amount",-10}});fail=false;
    frame=folded(world,1);
    check(frame.at("bank").at("currency")==60 && frame.at("character").at("state").at("currency")==40,"currency moves are bounded and atomic");
    {std::string ore;const auto bag=own();for(const auto& e:bag.at("inventory"))if(e.at("item")=="test_ore")ore=e.at("uid");
     world.action(1,{{"action","bank_deposit"},{"uid",ore},{"quantity",1}});}
    // Relog: the persisted row (state + bank) restores the same bank.
    const auto bank=saved.back().bank;world.remove(1);
    auto again=record(1);again["state"]=saved.back().state;again["bank"]=bank;world.add(1,1,1,again);
    check(!folded(world,1).contains("bank"),"bank contents stay private until a banker session opens");
    world.action(1,{{"action","bank_open"},{"target",banker}});
    check(folded(world,1).at("bank").at("items")==bank.at("items") && folded(world,1).at("bank").at("currency")==60 && !bank.at("items").empty(),"bank persists across relog");
    // Banka tabs ("Kasanız iki sekmeden oluşmaktadır"; 00440110/004401d0 page 0..1, 0043f330 cells page·64..): bank_slots cells per
    // tab, the state carries only the shown tab, entries keep their tab across relog; Kasa Erişimi opens the own bank anywhere.
    {const std::string a(32,'a'),b(32,'b'),c(32,'c'),d(32,'d');
     auto tabbed=record(1);tabbed["state"]=saved.back().state;
     tabbed["bank"]={{"items",{{{"uid",a},{"item","test_scroll"},{"quantity",1}},{{"uid",c},{"item","test_ore"},{"quantity",1}},{{"uid",b},{"item","test_scroll"},{"quantity",1},{"tab",1}}}},{"currency",60}};
     world.remove(1);world.add(1,1,1,tabbed);world.action(1,{{"action","bank_open"},{"target",banker}});
     frame=folded(world,1);
     check(frame.at("bank").at("tab")==0 && frame.at("bank").at("items").size()==2 && !frame.at("bank").at("items")[1].contains("tab"),"the first tab shows only its cells");
     world.action(1,{{"action","bank_deposit"},{"uid",uid(frame,"test_ingot")},{"quantity",1}});
     check(event(world.state(1),"error","bank"),"a full tab refuses a new cell (Banka dolu)");
     world.action(1,{{"action","bank_deposit"},{"uid",uid(frame,"test_ingot")},{"quantity",1},{"tab",1}});world.action(1,{{"action","bank_tab"},{"tab",1}});
     frame=folded(world,1);
     check(frame.at("bank").at("tab")==1 && frame.at("bank").at("items").size()==2 && count(frame.at("bank").at("items"),"test_ingot")==1 &&
           std::all_of(frame.at("bank").at("items").begin(),frame.at("bank").at("items").end(),[](const ikv::Json& e){return e.value("tab",0)==1;}) &&
           saved.back().bank.at("items").size()==4 && count(frame.at("character").at("state").at("inventory"),"test_ingot")==0,"a deposit lands on the requested second tab and persists");
     world.action(1,{{"action","bank_deposit"},{"uid",uid(frame,"test_rare")},{"quantity",1},{"tab",1}});check(event(world.state(1),"error","bank"),"the second tab has its own limit");
     world.action(1,{{"action","bank_tab"},{"tab",2}});check(event(world.state(1),"error","quantity"),"only two tabs exist");
     const auto stored=saved.back().bank;world.remove(1);auto back=record(1);back["state"]=saved.back().state;back["bank"]=stored;world.add(1,1,1,back);
     world.action(1,{{"action","bank_open"},{"target",banker}});world.action(1,{{"action","bank_tab"},{"tab",1}});
     frame=folded(world,1);
     check(count(frame.at("bank").at("items"),"test_ingot")==1 && frame.at("bank").at("items").size()==2,"tabs persist across relog");
     for(const auto& e:frame.at("bank").at("items"))if(e.at("item")=="test_ingot")world.action(1,{{"action","bank_withdraw"},{"uid",e.at("uid")},{"quantity",1}});
     frame=folded(world,1);
     check(count(frame.at("character").at("state").at("inventory"),"test_ingot")==1 && frame.at("bank").at("items").size()==1,"a second-tab cell withdraws into the bag");
     auto over=record(3);over["bank"]={{"items",{{{"uid",a},{"item","test_scroll"},{"quantity",1}},{{"uid",c},{"item","test_ore"},{"quantity",1}},{{"uid",d},{"item","test_rare"},{"quantity",1}}}},{"currency",0}};
     bool refused=false;try{world.add(3,3,3,over);}catch(...){refused=true;}check(refused,"a stored tab over its cell limit is refused at load");
     auto wrong=record(3);wrong["bank"]={{"items",{{{"uid",d},{"item","test_rare"},{"quantity",1},{"tab",2}}}},{"currency",0}};
     refused=false;try{world.add(3,3,3,wrong);}catch(...){refused=true;}check(refused,"a stored third tab is refused at load");
     // Kasa Erişimi (guide l.875-878): "Kasamı Göster" on the own panel; without the product it is refused, a revoke closes it.
     world.action(1,{{"action","bank_close"}});world.action(1,{{"action","bank_open"},{"target",1}});
     frame=folded(world,1);check(event(frame,"error","kasa") && !frame.contains("bank"),"own Kasamı Göster needs Kasa Erişimi");
     world.entitlement(1,false,true);world.action(1,{{"action","bank_open"},{"target",1}});
     frame=folded(world,1);check(frame.contains("bank") && frame.at("bank").at("banker")==1,"Kasa Erişimi opens the own bank without a banker");
     world.entitlement(1,false,false);check(!folded(world,1).contains("bank"),"revoking Kasa Erişimi closes the remote bank");}
    world.action(1,{{"action","recipe_forget"},{"recipe","test_scroll"}});check(own().at("recipes").empty(),"Seçili reçeteyi hafızamdan sil forgets the recipe");
}
// Eşyalar drag/split/destroy refusals (004344b0, 0042f690), vendor itemflag order, bound trade items and the Plus Paket entitlement.
void item_rules(ikv::Json config) {
    config["items"]["test_gem"]={{"name","Test Taşı"},{"stack",20},{"price",30},{"level",1}};
    config["items"]["test_relic"]={{"name","Test Emaneti"},{"stack",1},{"price",50},{"level",1},{"flags",3}};
    config["items"]["test_bound"]={{"name","Test Hatırası"},{"stack",1},{"price",40},{"level",1},{"flags",5}};
    config["items"]["test_clan"]={{"name","Test Klan Gözlüğü"},{"stack",1},{"price",0},{"clan_price",50},{"level",1},{"flags",68}};
    config["items"]["test_blade"]={{"name","Test Kılıcı"},{"stack",1},{"price",9},{"level",1},{"slot","weapon"}};
    config["quests"]=ikv::Json::object(); config["inventory_slots"]=7;
    const uint32_t merchant=2147493710u,banker=2147493711u,otomat=2147493712u;
    config["npcs"]={{{"id",merchant},{"kind","merchant"},{"name","Satıcı"},{"zone","eminonu"},{"x",0},{"y",100},{"hp",100},{"dialogue","Satıcı"},{"quests",ikv::Json::array()},{"shop",ikv::Json::array()}},
                    {{"id",banker},{"kind","questgiver"},{"banker",true},{"name","Bankacı"},{"zone","eminonu"},{"x",100},{"y",0},{"hp",100},{"dialogue","Kasa"},{"quests",ikv::Json::array()},{"shop",ikv::Json::array()}},
                    {{"id",otomat},{"kind","merchant"},{"no_buy",true},{"name","Otomat"},{"zone","eminonu"},{"x",0},{"y",-100},{"hp",100},{"dialogue","Otomat"},{"quests",ikv::Json::array()},{"shop",ikv::Json::array()}}};
    ikv::World world(config,[](std::span<const ikv::CharacterSave>){});
    const std::string gem(32,'a'),relic(32,'b'),bound(32,'c'),blade(32,'d'),clan(32,'e');
    auto player=record(1);player["state"]["inventory"]={{{"uid",gem},{"item","test_gem"},{"quantity",5}},{{"uid",relic},{"item","test_relic"},{"quantity",1}},
        {{"uid",bound},{"item","test_bound"},{"quantity",1}},{{"uid",blade},{"item","test_blade"},{"quantity",1}},{{"uid",clan},{"item","test_clan"},{"quantity",1}}};
    world.add(1,1,1,player);world.add(2,2,2,record(2));
    const auto own=[&](){return world.state(1).at("character").at("state");};
    const auto refused=[&](const char* code,const char* message){const auto frame=world.state(1);for(const auto& e:frame.at("events"))if(e.at("type")=="error" && e.at("code")==code && e.at("message")==message)return true;return false;};
    const auto sell=[&](const std::string& uid){world.action(1,{{"action","sell"},{"target",merchant},{"uid",uid},{"quantity",1}});};
    // Vendor: 1/5, then 1/3 once the account holds Plus; itemflag bit 2; Lira-less Klan Parası goods pay Klan Parası.
    const int64_t start=own().at("currency");
    sell(gem);check(own().at("currency")==start+6,"vendor pays a fifth");
    world.entitlement(1,true);sell(gem);check(own().at("currency")==start+16,"Plus Paket vendor pays a third");
    world.entitlement(1,false);sell(gem);check(own().at("currency")==start+22,"revoked Plus pays a fifth again");
    sell(relic);check(refused("sell","Bu eşya satılamaz."),"itemflag bit 2 cannot be sold (0x591f40)");
    sell(clan);check(own().value("clan_currency",0)==10,"itemflag-4 Klan Parası goods sell for Klan Parası");
    world.action(1,{{"action","equip"},{"uid",blade}});sell(blade);
    check(refused("equipped","Giyilmekte olan nesneler satılamaz; giysiyi önce üzerinizden çıkarın."),"worn items cannot be sold (0x591c98)");
    // classes.dat merchant=2 (entity flag +0x82&0x40): "Bu hedef, eşya satın almaz." before any item check.
    check(entity(world.state(1),otomat).value("no_buy",false),"no_buy flag projected");
    world.action(1,{{"action","sell"},{"target",otomat},{"uid",gem},{"quantity",1}});check(refused("sell","Bu hedef, eşya satın almaz."),"a no_buy merchant refuses to buy (0x591f54)");
    // Bag reorder (item_move): the dragged cell swaps with grid cell 2 of the unworn cells (gem, relic, bound); worn cells refuse.
    world.action(1,{{"action","item_move"},{"uid",gem},{"to",2}});
    {const auto bag=own().at("inventory");check(bag[2].at("uid")==gem && bag[0].at("uid")==bound,"item_move swaps the dragged cell with the target cell");}
    world.action(1,{{"action","item_move"},{"uid",blade},{"to",0}});check(refused("equipped","Giyilenler penceresi içerisinde taşıma yapamazsınız."),"worn cells do not move in the grid");
    // Ctrl-click item link (0042e8c0): the server sends the sender's own entry with its catalog definition; foreign uids are refused.
    world.action(1,{{"action","chat"},{"channel","local"},{"link",std::string(32,'9')}});check(refused("ownership","Bu eşya size ait değil."),"links need an owned item");
    world.action(1,{{"action","chat"},{"channel","local"},{"link",gem}});
    {bool linked=false;const auto frame=world.state(2);for(const auto& e:frame.at("events"))if(e.at("type")=="chat" && e.contains("link"))linked=e.at("link").at("item")=="test_gem" && e.at("link").at("definition").at("name")=="Test Taşı";
     check(linked,"an item link reaches nearby players with its definition");}
    // Destroy ('spro'): worn and itemflag-2 refusals, then the whole cell goes.
    world.action(1,{{"action","item_destroy"},{"uid",blade}});check(refused("equipped","Giyilmekte olan nesneler yok edilemez."),"worn items cannot be destroyed (0x591890)");
    world.action(1,{{"action","item_destroy"},{"uid",relic}});check(refused("destroy","Bu eşya yok edilemez."),"itemflag bit 2 cannot be destroyed (0x5918e0)");
    world.action(1,{{"action","item_destroy"},{"uid",gem}});
    {const auto bag=own().at("inventory");check(std::none_of(bag.begin(),bag.end(),[&](const ikv::Json& e){return e.at("uid")==gem;}),"destroy removes the whole cell");}
    // Bank: a worn cell refuses (0x591a8c); a bank cell can be destroyed.
    world.action(1,{{"action","bank_open"},{"target",banker}});world.action(1,{{"action","bank_deposit"},{"uid",blade},{"quantity",1}});
    check(refused("equipped","Giyilen bir eşyayı çıkarmadan bankaya koyamazsınız!"),"worn items cannot be banked (0x591a8c)");
    world.action(1,{{"action","bank_deposit"},{"uid",bound},{"quantity",1}});world.action(1,{{"action","bank_destroy"},{"uid",bound}});
    check(folded(world,1).at("bank").at("items").empty(),"bank cells can be destroyed");
    // Split ('mtes'): bag full (0x590ca0), refused during an exchange (0x590db0); bound items never enter an offer (0x591bbc).
    auto full=own();full["inventory"]={{{"uid",gem},{"item","test_gem"},{"quantity",5}}};for(int i=0;i<6;++i)full["inventory"].push_back({{"uid",std::string(31,'f')+char('0'+i)},{"item","test_bound"},{"quantity",1}});
    full["equipment"]=ikv::Json::object();world.remove(1);auto again=record(1);again["state"]=full;world.add(1,1,1,again);
    world.action(1,{{"action","split"},{"uid",gem},{"quantity",2}});check(refused("inventory","Çantanızda yer yok!!"),"split into a full bag (0x590ca0)");
    world.action(1,{{"action","trade_request"},{"target",2}});world.action(2,{{"action","trade_accept"},{"revision",terms(world.state(2)).at("revision")}});
    world.action(1,{{"action","split"},{"uid",gem},{"quantity",2}});check(refused("trade","Ticaret esnasında bunu yapamazsınız."),"split refused during an exchange (0x590db0)");
    world.action(1,{{"action","trade_offer"},{"items",ikv::Json::array({{{"uid",std::string(31,'f')+'0'},{"quantity",1}}})},{"currency",0}});
    check(refused("bound","Bu eşya size bağlanmıştır. Kimseye veremezsiniz."),"bound items cannot be offered (00431220)");
    world.action(1,{{"action","trade_cancel"}});
    // Şikayet Et: 3 per session, 10 with Plus.
    for(int i=0;i<3;++i)world.action(1,{{"action","complain"},{"target",2}});
    world.action(1,{{"action","complain"},{"target",2}});check(refused("complain","Şikayet hakkınız doldu."),"normal accounts have 3 complaints");
    world.entitlement(1,true);world.action(1,{{"action","complain"},{"target",2}});check(event(world.state(1),"ack"),"Plus accounts have 10 complaints");
}
// GBM-0 / GBM 0-B (guide): a level-based price offered on interact, confirmed by point_reset; Plus pays 1/3 and GBM 0-B needs Plus.
void point_reset_rules(ikv::Json config) {
    config["quests"]=ikv::Json::object(); config["point_reset"]={{"skills",1000},{"crafts",1000}};
    const uint32_t gbm=2147493720u,gbm_b=2147493721u;
    config["npcs"]={{{"id",gbm},{"kind","reset"},{"reset","skills"},{"name","GBM-0"},{"zone","eminonu"},{"x",0},{"y",100},{"hp",100},{"dialogue","GBM-0"},{"quests",ikv::Json::array()},{"shop",ikv::Json::array()}},
                    {{"id",gbm_b},{"kind","reset"},{"reset","crafts"},{"name","GBM 0-B"},{"zone","eminonu"},{"x",100},{"y",0},{"hp",100},{"dialogue","GBM 0-B"},{"quests",ikv::Json::array()},{"shop",ikv::Json::array()}}};
    ikv::World world(config,[](std::span<const ikv::CharacterSave>){});
    auto player=record(1);player["state"]["level"]=3;player["state"]["currency"]=10000;
    player["state"]["skills"]={{"depar",2}};player["state"]["resistances"]={{"ates",3}};player["state"]["crafts"]={{"madenci",2}};
    world.add(1,1,1,player);
    const auto own=[&](){return world.state(1).at("character").at("state");};
    const auto offer=[&](uint32_t machine){world.action(1,{{"action","interact"},{"target",machine}});const auto frame=world.state(1);for(const auto& e:frame.at("events"))if(e.at("type")=="reset_offer" && e.at("target")==machine)return e.at("price").get<int64_t>();return int64_t(-1);};
    check(offer(gbm)==3000,"GBM-0 offers its price for the character level");
    world.action(1,{{"action","point_reset"},{"target",gbm},{"price",2999}});check(event(world.state(1),"error","price") && own().at("skills").at("depar")==2,"a stale price is refused");
    world.action(1,{{"action","point_reset"},{"target",gbm},{"price",3000}});
    {const auto s=own();check(s.at("skills").empty() && s.at("resistances").empty() && s.at("crafts").at("madenci")==2 && s.at("currency")==7000,"GBM-0 resets yetenek and direnç points for the price");}
    world.action(1,{{"action","point_reset"},{"target",gbm},{"price",3000}});check(event(world.state(1),"error","reset"),"nothing left to reset");
    world.action(1,{{"action","interact"},{"target",gbm_b}});check(event(world.state(1),"error","plus"),"GBM 0-B needs Plus Paket");
    world.entitlement(1,true);check(offer(gbm_b)==1000,"Plus Paket pays a third");
    world.action(1,{{"action","point_reset"},{"target",gbm_b},{"price",1000}});
    {const auto s=own();check(s.at("crafts").empty() && s.at("currency")==6000,"GBM 0-B resets üretim and toplama points");}
}
const ikv::Json& entity(const ikv::Json& state, uint32_t id) {
    for(const auto& value:state.at("entities"))if(value.at("id")==id)return value;
    throw std::runtime_error("visible entity missing");
}
bool authority_event(const ikv::Json& state,const char* type,uint32_t entity_id,uint32_t source,uint32_t target) {
    for(const auto& value:state.at("events"))if(value.at("type")==type) {
        for(const char* key:{"entity","source","target"})
            check(value.at(key).is_number_integer() && value.at(key).get<uint64_t>()>0 &&
                  value.at(key).get<uint64_t>()<=UINT32_MAX,"projected event uses positive uint32 entity IDs");
        if(value.at("entity")==entity_id && value.at("source")==source && value.at("target")==target)return true;
    }
    return false;
}
void visible_cosmetics(const ikv::Json& mechanics) {
    auto config=mechanics;
    config["npcs"][0]["source_uid"]=1001;config["npcs"][0]["source_class"]=16;
    config["npcs"][0]["appearance"]={{"sex",1},{"face",2}};
    for(int n=1;n<=2;++n){config["npcs"][n].erase("source_uid");config["npcs"][n].erase("source_class");}
    ikv::World world(config,[](std::span<const ikv::CharacterSave>){});
    const auto visual=ikv::Json::array({{{"slot",1},{"source_uid",4002}}});
    auto appearances=ikv::Json::array();
    for(int id=1;id<=3;++id) {
        auto player=record(id,id*100);player["class_id"]=id;
        player["appearance"]={{"sex",id%2},{"face",id},{"hair",id},{"hair_color",1},{"beard",1},{"beard_color",1}};
        appearances.push_back(player.at("appearance"));
        world.add(id,id,id,player);
    }
    world.add(4,4,4,record(4,4000));
    for(int observer=1;observer<=3;++observer) {
        auto frame=world.state(observer);
        check(frame.at("zone")=="eminonu" && frame.at("source_registry_id").is_null() &&
              frame.at("bounds")==ikv::Json::array({-10000,-10000,10000,10000}),"independent zone metadata at state root");
        { // VOD-fitted curve: 1500·1.122^(L−1) per level ("Puan 424/1500" at 1, "1811/3183" at 2); Puan shows cumulative XP.
            const int level=frame.at("character").at("state").at("level");const auto& f=config.at("formulas");
            const auto need=[&](int l){return int(std::round(f.at("xp_first").get<double>()*std::pow(f.at("xp_growth").get<double>(),l-1)));};
            int64_t before=0;for(int l=1;l<level;++l)before+=need(l);
            check(need(1)==1500 && need(1)+need(2)==3183 && frame.at("xp_interval")==need(level) && frame.at("xp_total")==before,"root XP interval follows the VOD level curve");
            const auto& stats=frame.at("stats");const int cls=frame.at("character").at("class_id");
            // 00402f60: Saldırı = Seviye·class_attack + Beceri (+Efsun/buffs), Savunma likewise; Zırh capped at 60 without stances.
            // Beceri: VOD samples (level 1 Büyücü Saldırı 21.5 = 1.5 + 20.0), then the line.
            const auto& bc=f.at("beceri");const auto& bl=bc.at("levels");
            const double beceri=level<=int(bl.size())?bl[size_t(level-1)].get<double>():bc.at("line")[0].get<double>()+bc.at("line")[1].get<double>()*level;
            check(bl.at(0)==20.0 && std::lround((1.5+bl.at(0).get<double>())*10)==215,"level-1 Büyücü Saldırı matches VOD 10:00 (21.5)");
            check(stats.at("attack").get<long>()>=std::lround((level*f.at("class_attack")[cls-1].get<double>()+beceri)*10) &&
                  stats.at("defense").get<long>()>=std::lround((level*f.at("class_defense")[cls-1].get<double>()+beceri)*10) &&
                  stats.at("armor")[1]==60 && stats.at("armor")[0].get<int>()<=600,"Karakter Bilgisi Saldırı/Savunma/Zırh follow the client formulas");
        }
        check(frame.at("character").at("class_id")==observer &&
              frame.at("character").at("appearance")==appearances.at(observer-1) &&
              frame.at("character").at("equipment")==visual,"self class cosmetics and visual equipment forwarded");
        for(int other=1;other<=3;++other)if(other!=observer) {
            const auto& remote=entity(frame,other);
            check(remote.at("class_id")==other && remote.at("appearance")==appearances.at(other-1) &&
                  remote.at("equipment")==visual && remote.at("alive")==true,"every visible player has cosmetics equipment and life state");
            check(!remote.contains("state") && !remote.contains("inventory"),"remote private inventory is never forwarded");
        }
        const auto& npc=entity(frame,2147493649u);
        check(npc.at("source_uid")==1001 && npc.at("source_class")==16 && npc.at("appearance")==config.at("npcs")[0].at("appearance") &&
              npc.at("equipment").empty() && npc.at("alive")==true,"NPC source metadata cosmetics equipment and life state forwarded");
        check(entity(frame,2147493650u).at("source_uid")==1002 && entity(frame,2147493650u).at("source_class")==17 &&
              entity(frame,2147493651u).at("source_uid")==16001 && entity(frame,2147493651u).at("source_class")==9,
              "absent NPC source metadata defaults per kind, merchant distinct from questgiver");
    }
    check(world.state(4).at("character").at("appearance").empty(),"empty appearance remains valid");
    world.action(1,{{"action","unequip"},{"slot","weapon"}});
    auto unequipped=world.state(2);
    check(authority_event(unequipped,"unequip",1,1,1) && entity(unequipped,1).at("equipment").empty(),
          "unequip projects authoritative identity and cleared visual slots");
    for(const auto& value:unequipped.at("events"))if(value.at("type")=="unequip")
        check(value.at("equipment").empty(),"unequip event clears source visual identity");
    check(!event(world.state(4),"unequip"),"visual events do not escape AOI");
    world.action(1,{{"action","equip"},{"uid",uid(world.state(1),"training_blade")}});
    auto equipped=world.state(2);
    check(authority_event(equipped,"equip",1,1,1) && entity(equipped,1).at("equipment")==visual,
          "equip projects authoritative entity rather than inventory UID");
    for(const auto& value:equipped.at("events"))if(value.at("type")=="equip")
        check(value.at("equipment")==visual,"equip event contains only source visual identity");
    for(const auto& appearance:ikv::Json::array({{{"sex",2}},{{"sex",1},{"hair",18}},{{"sex",0},{"hair",21}},{{"face",10}}})) {
        auto invalid=record(5);invalid["appearance"]=appearance;bool rejected=false;
        try{world.add(5,5,5,invalid);}catch(const std::invalid_argument&){rejected=true;}
        check(rejected && world.state(5).empty(),"invalid sex and cosmetic indices rejected before admission");
    }
}
void synthetic_support_navigation(const ikv::Json& mechanics) {
    // SYNTHETIC NOT ORIGINAL: finite zero heights and hand-admitted cells test mechanics only.
    auto grid=std::make_shared<ikv::Zone>();grid->width=grid->height=2048;
    grid->source="SYNTHETIC NOT ORIGINAL";grid->cell_size=1.5f;
    grid->cells.assign(2048u*2048u,0);grid->heights.assign(grid->cells.size(),0.0f);
    for(int y=-2;y<=2;++y)for(int x=0;x<=12;++x)
        grid->cells[size_t(y+1024)*2048+size_t(x+1024)]=255;
    grid->cells[1024u*2048u+1025u]=0;
    auto config=mechanics;
    config["zones"]={{"eminonu",{{"source_registry_id",0},{"bounds",{-1536000,-1536000,1534500,1534500}},
                               {"spawn",{0,0}},{"collision",ikv::Json::array()}}}};
    config["npcs"]=ikv::Json::array();config["quests"]=ikv::Json::object();config["zones"]["old_town"]=mechanics.at("zones").at("old_town"); // the fixture arena zone
    ikv::World world(config,[](std::span<const ikv::CharacterSave>){});
    bool missing=false;try{world.add(1,1,1,record(1));}catch(const std::invalid_argument&){missing=true;}
    check(missing && world.state(1).empty(),"source admission requires installed support grid");
    world.set_source_zones({{"eminonu",grid}});world.add(1,1,1,record(1));
    auto root=world.state(1);
    check(root.at("zone")=="eminonu" && root.at("source_registry_id")==0 &&
          root.at("bounds")==config.at("zones").at("eminonu").at("bounds"),"source metadata at state root");
    world.destination(1,1500,0,1);
    check(event(world.state(1),"correction"),"unsupported source destination rejected");
    world.destination(1,6000,0,2);
    bool detoured=false,diagonal=false;int x=0,y=0;
    for(int tick=0;tick<150;++tick) {
        world.advance();auto frame=world.state(1);const auto& state=frame.at("character").at("state");
        const int nx=state.at("x"),ny=state.at("y");
        check(!event(frame,"correction"),"supported eight-neighbor smoothed detour never corrects");
        for(int sample=0;sample<=10;++sample)
            check(ikv::zone_height(*grid,float(x+(nx-x)*sample/10)/1000.0f,float(y+(ny-y)*sample/10)/1000.0f).has_value(),
                  "synthetic route never crosses unsupported cell");
        detoured=detoured || ny!=0;diagonal=diagonal || (nx!=x && ny!=y);x=nx;y=ny;
        if(x==6000 && y==0)break;
    }
    check(detoured && diagonal && x==6000 && y==0,"eight-neighbor route reaches goal around blocked source cell");
    world.destination(1,18000,0,3);ticks(world,120);
    check(world.state(1).at("character").at("state").at("x")==18000,"source footprint supports coordinates beyond independent 10000 bound");
    const auto rejected_provider=[&](const ikv::Json& candidate) {
        bool rejected=false;
        try{ikv::World invalid(candidate,[](std::span<const ikv::CharacterSave>){});invalid.set_source_zones({{"eminonu",grid}});}
        catch(const std::invalid_argument&){rejected=true;}
        check(rejected,"provider rejects unsupported configured spawn/default/NPC/patrol/portal point");
    };
    auto bad=config;bad["zones"]["eminonu"]["spawn"]={1500,0};rejected_provider(bad);
    bad=config;bad["defaults"]["x"]=1500;rejected_provider(bad);
    auto patrol=config;patrol["npcs"]=ikv::Json::array({mechanics.at("npcs")[2]});
    patrol["npcs"][0]["x"]=0;patrol["npcs"][0]["y"]=-3000;patrol["npcs"][0]["patrol"]={{0,-3000},{0,-1500}};
    ikv::World admitted(patrol,[](std::span<const ikv::CharacterSave>){});admitted.set_source_zones({{"eminonu",grid}});
    bad=patrol;bad["npcs"][0]["x"]=1500;bad["npcs"][0]["y"]=0;rejected_provider(bad);
    bad=patrol;bad["npcs"][0]["patrol"]={{1500,0}};rejected_provider(bad);
    // <loc> doors: using one moves the user to its arrival; an instanced zone ("instances": registry rows) gives each group its own copy.
    auto portals=config;
    portals["zones"]["migrat"]={{"source_registry_id",36},{"instances",{27,28}},{"bounds",{-1536000,-1536000,1534500,1534500}},{"spawn",{0,0}},{"collision",ikv::Json::array()}};
    const ikv::Json door={{"id",2147493700u},{"kind","door"},{"name","Kapı"},{"zone","eminonu"},{"x",15000},{"y",0},{"hp",100},{"dialogue","Kapı"},
                          {"quests",ikv::Json::array()},{"shop",ikv::Json::array()},{"door",{{"to","migrat"},{"arrive",{6000,0}}}}};
    auto rat=mechanics.at("npcs")[2];rat["zone"]="migrat";rat["x"]=5000;rat["y"]=0;rat["patrol"]=ikv::Json::array(); // within aoi of the arrival
    portals["npcs"]={door,rat};
    ikv::World portal_admitted(portals,[](std::span<const ikv::CharacterSave>){});portal_admitted.set_source_zones({{"eminonu",grid},{"migrat",grid}});
    for(uint32_t p=1;p<=5;++p)portal_admitted.add(p,p,p,record(int(p),14900)); // within interaction_range of the door
    for(const auto& [leader,member]:{std::pair{1u,2u},{4u,5u}}){portal_admitted.action(leader,{{"action","group_invite"},{"target",member}});portal_admitted.action(member,{{"action","group_accept"}});}
    for(uint32_t p=1;p<=5;++p)portal_admitted.action(p,{{"action","interact"},{"target",2147493700u}});
    std::array<ikv::Json,6> after;for(uint32_t p=1;p<=5;++p)after[p]=portal_admitted.state(p);
    const auto zone_of=[&](uint32_t p){return after[p].at("zone").get<std::string>();};
    check(zone_of(1)=="migrat1" && zone_of(2)=="migrat1" && zone_of(3)=="migrat" && zone_of(4)=="migrat2" && zone_of(5)=="migrat2",
          "a door sends each group to its own instance copy and solo players to the shared zone");
    check(after[1].at("source_registry_id")==27 && after[4].at("source_registry_id")==28 && after[1].at("character").at("state").at("x")==6000,"instance copy uses its registry row and the door arrival");
    bool copy_rat=false;for(const auto& e:after[1].at("entities"))copy_rat=copy_rat || e.at("id")==2147493651u+65536u;
    check(copy_rat,"instance copy has its own NPC population");
    portal_admitted.remove(1);auto returning=record(1,6000);returning["state"]["zone"]="migrat1";portal_admitted.add(1,1,1,returning);
    check(portal_admitted.state(1).at("zone")=="migrat","a returning character resumes in the shared zone, not a group's copy");
    bad=config;bad["npcs"]={door};bad["npcs"][0]["door"]={{"to","eminonu"},{"arrive",{1500,0}}};rejected_provider(bad);
    bad=config;bad["zones"]["eminonu"]["bounds"]={-10000,-10000,10000,10000};rejected_provider(bad);
    bad=config;bad["zones"]["eminonu"]["source_registry_id"]=71;rejected_provider(bad);
}
// A player's click beyond the NPC-step A* budget: SYNTHETIC NOT ORIGINAL field (cells −60..60) split by a wall (x = 5, |y| ≤ 40)
// whose detour pops ~2900 nodes (Eminönü spawn: Teşkilat Hattı 4047, the Fare 6.5k). An NPC behind the wall is walked to and
// talked to; a ground click at an unreachable walkable cell walks toward it instead of not moving at all.
void click_routes(const ikv::Json& mechanics) {
    auto grid=std::make_shared<ikv::Zone>();grid->width=grid->height=2048;
    grid->source="SYNTHETIC NOT ORIGINAL";grid->cell_size=1.5f;
    grid->cells.assign(2048u*2048u,0);grid->heights.assign(grid->cells.size(),0.0f);
    for(int y=-60;y<=60;++y)for(int x=-60;x<=60;++x)if(x!=5 || std::abs(y)>40)grid->cells[size_t(y+1024)*2048+size_t(x+1024)]=255;
    grid->cells[1024u*2048u+1124u]=255; // an unconnected walkable cell at (150000,0)
    auto config=mechanics;config["aoi"]=200000;config["movement_step"]=800;
    config["zones"]={{"eminonu",{{"source_registry_id",0},{"bounds",{-1536000,-1536000,1534500,1534500}},{"spawn",{0,0}},{"collision",ikv::Json::array()}}}};
    config["zones"]["old_town"]=mechanics.at("zones").at("old_town");config["quests"]=ikv::Json::object();
    auto giver=mechanics.at("npcs")[0];giver["x"]=15000;giver["y"]=0;giver["quests"]=ikv::Json::array();config["npcs"]={giver};
    ikv::World world(config,[](std::span<const ikv::CharacterSave>){});world.set_source_zones({{"eminonu",grid}});
    world.add(1,1,1,record(1));world.state(1);
    world.action(1,{{"action","interact"},{"target",giver.at("id")}});
    bool talked=false,refused=false;
    for(int tick=0;tick<600 && !talked && !refused;++tick){world.advance();const auto frame=world.state(1);talked=event(frame,"dialogue");refused=event(frame,"error","range");}
    check(talked && !refused,"a click on an NPC behind a long detour walks there and talks");
    world.remove(1);world.add(1,1,1,record(1));world.state(1);
    world.destination(1,150000,0,1);ticks(world,40);const auto frame=world.state(1);
    check(!event(frame,"correction") && frame.at("character").at("state").at("x").get<int>()>0,"a ground click at an unreachable cell walks toward it");
}
// <loc> door gates: requiredquestid (NONCANONICAL: the quest taken or done) and gmonly.
void door_rules(ikv::Json config) {
    config["quests"]["first_hunt"]["source_quest_id"]=72;
    for(auto& n:config["npcs"])if(n.at("id")==2147493652u)n["door"]["required_quest"]=72;else if(n.at("id")==2147493653u)n["door"]["gmonly"]=true;
    ikv::World world(config,[](std::span<const ikv::CharacterSave>){});
    world.add(1,1,1,record(1,150));world.add(2,2,2,record(2,150),true);
    const auto use=[&](uint32_t id,uint32_t door){world.action(id,{{"action","interact"},{"target",door}});ticks(world,25);}; // walks to the door, then uses it
    use(1,2147493652u);
    check(event(world.state(1),"error","quest") && world.state(1).at("zone")=="eminonu","quest-gated door refuses without the quest");
    world.destination(1,150,0,1);ticks(world,20);
    world.action(1,{{"action","quest_accept"},{"target",2147493649u},{"quest","first_hunt"}});
    use(1,2147493652u);
    check(world.state(1).at("zone")=="old_town","quest-gated door opens once the quest is taken");
    use(1,2147493653u);
    check(event(world.state(1),"error","permission") && world.state(1).at("zone")=="old_town","gmonly door refuses players");
    world.action(2,{{"action","zone"},{"zone","old_town"}});use(2,2147493653u);
    check(world.state(2).at("zone")=="eminonu","gmonly door admits administrators");
    // social.guild_zones (TopkapiPage.aspx "Yalnızca loncaların girebildiği"): a lonca member enters, others are refused.
    config["social"]["guild_zones"]={"old_town"};
    ikv::World hub(config,[](std::span<const ikv::CharacterSave>){});hub.add(1,1,1,record(1,150));hub.add(3,3,3,record(3,150));
    for(uint32_t id:{1u,3u})hub.action(id,{{"action","quest_accept"},{"target",2147493649u},{"quest","first_hunt"}});
    hub.action(3,{{"action","guild_create"},{"name","Kervan"}});
    for(uint32_t id:{1u,3u})hub.action(id,{{"action","interact"},{"target",2147493652u}});
    ticks(hub,25);
    check(event(hub.state(1),"error","guild") && hub.state(1).at("zone")=="eminonu","the Lonca Merkezi refuses players outside a lonca");
    check(hub.state(3).at("zone")=="old_town","a lonca member enters the Lonca Merkezi");
}
// Bağlantılar ignore list, Kabadayı / enemy-clan PvP consent, Klan recruiters and channel, emotes.
void social_rules(ikv::Json config) {
    for(int r:{0,1}){config["npcs"][r]["clan"]=r+1;config["npcs"][r]["recruiter"]=true;} // recruiters at x=200 (Arzın Çocukları) and x=400 (Lodos)
    ikv::World world(config,[](std::span<const ikv::CharacterSave>){});
    const auto leveled=[](int id,int x){auto r=record(id,x);r["state"]["level"]=5;return r;}; // guide: clans from level 5
    world.add(1,1,1,leveled(1,300));world.add(2,2,2,record(2,250));world.add(3,3,3,leveled(3,350));
    const auto chat=[](const ikv::Json& state){for(const auto& e:state.at("events"))if(e.at("type")=="chat")return true;return false;};
    world.action(2,{{"action","contact_add"},{"list","ignored"},{"target",1}});
    check(world.state(2).at("character").at("state").at("contacts").at("ignored").size()==1,"Yasaklılara Ekle");
    world.action(1,{{"action","chat"},{"channel","local"},{"text","selam"}});
    check(!chat(world.state(2)) && chat(world.state(3)),"ignored speaker reaches others only");
    ticks(world,10);world.action(1,{{"action","chat"},{"channel","private"},{"to",2},{"text","fısıltı"}});
    check(!chat(world.state(2)) && chat(world.state(1)),"ignored whisper is dropped");
    world.action(3,{{"action","chat"},{"channel","local"},{"text","merhaba"}});
    check(chat(world.state(2)),"unlisted speaker still heard");
    world.action(2,{{"action","contact_add"},{"list","friends"},{"target",1}});
    const auto listed=world.state(2);const auto& contacts=listed.at("character").at("state").at("contacts");
    check(contacts.at("ignored").empty() && contacts.at("friends").size()==1 && listed.at("contacts_online")==ikv::Json::array({1}),"friend moves off ignore list and shows online");
    world.action(2,{{"action","contact_remove"},{"list","friends"},{"character_id",1}});
    check(world.state(2).at("character").at("state").at("contacts").at("friends").empty(),"Listeden sil");
    world.action(1,{{"action","attack"},{"target",2}});
    check(event(world.state(1),"error","target"),"no PvP without consent");
    world.action(1,{{"action","kabadayi"}});world.action(1,{{"action","attack"},{"target",2}});
    check(event(world.state(1),"error","target"),"one-sided Kabadayı is not consent");
    world.action(2,{{"action","kabadayi"}});
    check(entity(world.state(1),2).at("kabadayi")==true,"Kabadayı flag projected for name colour");
    world.action(1,{{"action","attack"},{"target",2}});check(event(world.state(1),"ack"),"both Kabadayı may fight");
    ticks(world,600);
    const auto victim=world.state(2);
    check(victim.at("character").at("state").at("hp")==0 && !victim.at("character").at("state").contains("kabadayi"),"PvP kill ends Kabadayı mode");
    world.add(9,9,9,record(9,250));world.action(9,{{"action","clan_join"},{"target",2147493649u}});check(event(world.state(9),"error","level") && !world.state(9).at("character").at("state").contains("clan"),"Klana Katıl needs level 5 (guide)");
    world.action(3,{{"action","clan_join"},{"target",2147493649u}});world.action(1,{{"action","clan_join"},{"target",2147493650u}});
    check(world.state(3).at("character").at("state").at("clan")==1 && world.state(1).at("character").at("state").at("clan")==2,"Klana Katıl");
    world.action(3,{{"action","clan_join"},{"target",2147493650u}});check(event(world.state(3),"error","clan"),"one clan at a time");
    world.action(3,{{"action","chat"},{"channel","clan"},{"text","klan"}});
    check(chat(world.state(3)) && !chat(world.state(1)),"Klan channel stays inside the clan");
    world.action(3,{{"action","attack"},{"target",1}});check(event(world.state(3),"ack"),"enemy clans are hostile");
    ticks(world,600);
    check(world.state(1).at("character").at("state").at("hp")==0 && world.state(3).at("character").at("state").at("san")==1 && world.state(1).at("character").at("state").at("karizma")==80,"enemy-clan kill earns guide Şan (1 below 3000) and costs the victim 20% Karizma");
    world.action(3,{{"action","clan_leave"},{"target",2147493650u}});check(event(world.state(3),"error","clan"),"leave only at own recruiter");
    world.action(3,{{"action","clan_leave"},{"target",2147493649u}});
    const auto left=world.state(3).at("character").at("state");
    check(!left.contains("clan") && !left.contains("san") && !left.contains("clan_currency"),"Klandan Ayrıl forfeits Şan and Klan Parası");
    auto safe=config;safe["social"]["safe_zones"]={"eminonu"};
    ikv::World town(safe,[](std::span<const ikv::CharacterSave>){});
    auto flagged=record(4,300);flagged["state"]["kabadayi"]=true;town.add(4,4,4,flagged);town.add(5,5,5,record(5,350));
    check(!town.state(4).at("character").at("state").contains("kabadayi"),"Kabadayı does not survive reconnect");
    town.action(4,{{"action","kabadayi"}});town.action(5,{{"action","kabadayi"}});town.action(4,{{"action","attack"},{"target",5}});
    check(event(town.state(4),"error","target"),"safe zone forbids PvP");
    town.action(5,{{"action","chat"},{"channel","roleplay"},{"text","/otur"}});
    bool sat=false;const auto observed=town.state(4);for(const auto& e:observed.at("events"))sat|=e.at("type")=="emote" && e.at("entity")==5 && e.at("clip")==82;
    check(sat,"/otur plays RP_Cok for observers");
    ticks(town,10);town.action(5,{{"action","chat"},{"channel","roleplay"},{"text","/yok"}});check(event(town.state(5),"error","emote"),"unknown emote");
}
// Klan OOK guards (guide "Klan OOK'leri"), Şan level falloff, clan rank and lonca progression (numbers NONCANONICAL: social.san_falloff,
// social.clan_ranks, economy.guild_level_cost).
void clan_rules(ikv::Json config) {
    const uint32_t arz=2147493660u,lodos=2147493661u;
    const auto guard=[&](uint32_t id,int clan,int x,const char* kind){auto n=config["npcs"][0];n.update({{"id",id},{"kind",kind},{"name",clan==1?"Arz Muhafızı":"Tüccar (Lodos)"},{"x",x},{"clan",clan},{"hp",50},
        {"damage",7},{"attack_cooldown",20},{"aggro",300},{"leash",1000},{"speed",25},{"respawn",200},{"xp",1},{"currency",1},{"patrol",ikv::Json::array()},{"loot",ikv::Json::array()},{"quests",ikv::Json::array()}});return n;};
    config["npcs"].push_back(guard(arz,1,-2000,"questgiver"));config["npcs"].back()["recruiter"]=true;
    config["npcs"].push_back(guard(lodos,2,3000,"merchant"));config["npcs"].back()["shop"]={"healing_tonic"};
    ikv::World world(config,[](std::span<const ikv::CharacterSave>){});
    const auto pc=[&](uint32_t id,int x,int clan,int level,int64_t san=0,int hp=0){auto r=record(int(id),x);if(clan)r["state"]["clan"]=clan;r["state"]["level"]=level;r["state"]["san"]=san;
        r["state"]["clan_currency"]=5000;if(hp)r["state"]["hp"]=hp;world.add(id,id,id,r);};
    const auto own=[&](uint32_t p){return world.state(p).at("character").at("state");};
    const auto hp_of=[&](uint32_t viewer,uint32_t id)->int{const auto frame=world.state(viewer);for(const auto& e:frame.at("entities"))if(e.at("id")==id)return e.at("hp");return -1;};
    const auto until=[&](const auto& done,const char* message){for(int t=0;t<800 && !done();++t)world.advance();check(done(),message);};
    pc(1,-1800,2,10);pc(2,-1880,1,10);pc(3,-1900,0,10);
    const int full1=own(1).at("hp"),full2=own(2).at("hp"),full3=own(3).at("hp");
    check(entity(world.state(3),arz).value("recruiter",false) && entity(world.state(3),arz).at("clan")==1,"recruiter flag and clan projected");
    ticks(world,60);
    check(own(1).at("hp").get<int>()<full1 && own(2).at("hp")==full2 && own(3).at("hp")==full3,"a clan guard bites the enemy-clan player only");
    world.action(1,{{"action","interact"},{"target",arz}});check(event(world.state(1),"error","target"),"the other clan cannot talk to a hostile Klan OOK");
    world.action(3,{{"action","interact"},{"target",arz}});check(!event(world.state(3),"error"),"a clanless player still talks to the recruiter");
    world.action(2,{{"action","attack"},{"target",arz}});check(event(world.state(2),"error","target"),"a clan cannot strike its own guard");
    world.action(1,{{"action","attack"},{"target",arz}});
    until([&]{return hp_of(1,arz)==0;},"an enemy-clan player fells the guard");
    world.action(2,{{"action","interact"},{"target",arz}});check(event(world.state(2),"error","dead"),"a felled Klan OOK serves no one until it rises");
    ticks(world,220);check(hp_of(2,arz)==50,"the guard rises at its post");
    // Şan falls with the level difference: full up to 5 levels above, 10 levels between 5 and 15 (9 above: 6/10 of 100 = 60), none from 15.
    pc(4,-300,1,10,3000);pc(5,-200,2,1,3000,5);pc(6,300,1,20,3000);pc(7,400,2,1,3000,5); // clear of the guard (-2000) and the rat (800)
    world.action(4,{{"action","attack"},{"target",5}});until([&]{return own(5).at("hp")==0;},"low-level enemy falls");
    check(own(4).at("san")==3060 && own(4).at("clan_currency")==5060 && own(5).at("karizma")==80,"9 levels above the victim earns 60 of 100 Şan and Klan Parası");
    world.action(6,{{"action","attack"},{"target",7}});until([&]{return own(7).at("hp")==0;},"far lower enemy falls");
    check(own(6).at("san")==3000 && own(6).at("clan_currency")==5000 && own(7).at("karizma")==80,"15+ levels above earns no Şan");
    check(world.state(4).at("stats").at("clan_rank")=="Ağa" && !world.state(3).at("stats").contains("clan_rank"),"clan rank by Şan; none without a clan");
    // Lonca: Klan Parası donations fill Lonca Parası; level L rises at L × guild_level_cost (1000): 3000 takes level 1 to 3.
    world.action(4,{{"action","guild_create"},{"name","Kervan"}});
    world.action(4,{{"action","guild_donate"},{"amount",999999}});check(event(world.state(4),"error","clan_currency"),"donation needs the Klan Parası");
    world.action(4,{{"action","guild_donate"},{"amount",3000}});
    const auto g=own(4).at("guild");
    check(g.at("level")==3 && g.value("funds",-1)==0 && own(4).at("clan_currency")==2060 && world.state(4).at("stats").at("guild_rank")=="Lonca Reisi","donated Klan Parası raises the lonca level");
}
}
// Worst-case crowd: 32 players in one cell with fully equipped prefixed weapons. Every frame stays inside the protocol
// envelope, and self plus the five other group members are always present (farthest strangers may drop out).
// Gear: a worn prefix feeds Karakter Bilgisi Efsun until its Eskime is exhausted.
void crowd_and_gear(ikv::Json config) {
    int prefix=0;for(const auto& [id,p]:config.at("prefixes").items())if(p.at("attributes")[0][0]==39){prefix=std::stoi(id);break;}
    check(prefix>0,"generated Efsun prefix table has a Maksimum Hasar prefix");
    config["items"]["training_blade"]["durability"]=1000;
    ikv::World world(config,[](std::span<const ikv::CharacterSave>){});
    for(int id=1;id<=32;++id) {
        auto player=record(id,id%3);
        const std::string item_uid=std::string(30,'a')+(id<10?"0":"")+std::to_string(id);
        player["state"]["inventory"]=ikv::Json::array({{{"uid",item_uid},{"item","training_blade"},{"quantity",1},{"prefixes",{prefix,prefix}}}});
        world.add(uint32_t(id),id,id,player);
    }
    for(int member=2;member<=6;++member){world.action(1,{{"action","group_invite"},{"target",member}});world.action(uint32_t(member),{{"action","group_accept"}});}
    for(int observer=1;observer<=32;++observer) {
        const auto frame=world.state(uint32_t(observer));
        check(frame.dump().size()<=65533,"crowded frame stays inside the protocol envelope");
        if(observer<=6)for(int member=1;member<=6;++member)if(member!=observer)entity(frame,uint32_t(member));
    }
    const auto worn=world.state(1);
    check(worn.at("stats").at("efsun").contains("39") && worn.at("stats").at("weapon_damage")[1].get<int>()>worn.at("stats").at("weapon_damage")[0].get<int>(),"worn prefix feeds Karakter Bilgisi Efsun and Silah Hasarı");
    check(entity(world.state(2),1).at("equipment")[0].at("prefixes")==ikv::Json::array({prefix,prefix}),"worn weapon prefixes travel with the visual for the glow");
    auto broken=record(40);broken["state"]["inventory"]=ikv::Json::array({{{"uid",std::string(32,'b')},{"item","training_blade"},{"quantity",1},{"prefixes",{prefix}},{"durability",0}}});
    ikv::World other(config,[](std::span<const ikv::CharacterSave>){});other.add(40,40,40,broken);
    check(other.state(40).at("stats").at("efsun").empty() && !entity(world.state(2),1).is_null(),"exhausted Eskime gives no Efsun");
    // A catalog-heavy AOI fills the frame with optional NPCs; every queued notice is still delivered (events are reserved
    // before optional entities, which are resent next frame anyway).
    auto heavy=config;ikv::Json merchant;for(const auto& n:config.at("npcs"))if(n.at("id")==2147493650u)merchant=n;
    for(int m=0;m<64;++m) {
        auto n=merchant;n["id"]=2147494650u+uint32_t(m);n["shop"]=ikv::Json::array();
        for(int i=0;i<=m%20;++i){const auto id="crowd_"+std::to_string(m*20+i);auto item=config.at("items").at("healing_tonic");item["name"]=std::string(48,'x')+std::to_string(m*20+i);heavy["items"][id]=item;n["shop"].push_back(id);}
        heavy["npcs"].push_back(n);
    }
    ikv::World market(heavy,[](std::span<const ikv::CharacterSave>){});market.add(1,1,1,record(1));
    for(int i=0;i<30;++i)market.action(1,{{"action","quest_abandon"},{"quest","first_hunt"}});
    const auto full=market.state(1);int notices=0;for(const auto& e:full.at("events"))notices+=e.at("type")=="error";
    check(full.dump().size()>60000 && full.dump().size()<=65533 && notices==30,"a frame filled by nearby catalogs still delivers every queued notice");
}
// Hızlı Erişim Çubuğu persistence, Boz Ayı slot swap + summon fight/redirect/expiry, PvP skills and detail-text procs.
void actionbar_rules(ikv::Json config) {
    config["npcs"][2]["hp"]=100000; // a rat that survives to retaliate against the summon
    config["skills"]["buz_oku"]["crit"]=ikv::Json::array({1,100}); // deterministic crit (detail-text rows otherwise roll)
    config["skills"]["kanatma"]["procs"]["drain"]=ikv::Json::array({ikv::Json::array({1,100})});
    std::vector<ikv::CharacterSave> saved;
    ikv::World world(config,[&](std::span<const ikv::CharacterSave> s){saved.assign(s.begin(),s.end());});
    const auto pool=[](ikv::Json r){r["state"]["level"]=30;r["state"]["mana"]=r["state"]["hp"]=1000000;return r;}; // level-30 pools (maxima derive from level), filled
    auto warrior=pool(record(1));warrior["state"]["skills"]={{"kanatma",5}};world.add(1,1,1,warrior);
    const int full_hp=world.state(1).at("character").at("state").at("max_hp");
    const int after_cast=world.state(1).at("character").at("state").at("mana").get<int>()-config.at("skills").at("boz_ayi").at("mana").get<int>();
    const auto own=[&](uint32_t id){return world.state(id).at("character").at("state");};
    const auto tonic=uid(world.state(1),"healing_tonic");
    ikv::Json slots=ikv::Json::array();for(int i=0;i<10;++i)slots.push_back(nullptr);
    slots[0]={{"skill","boz_ayi"}};slots[3]={{"item",tonic}};
    world.action(1,{{"action","quickbar"},{"slots",slots}});
    check(own(1).at("quickbar")==slots,"quickbar layout stored in character state");
    world.action(1,{{"action","quickbar"},{"slots",ikv::Json::array({nullptr})}});check(event(world.state(1),"error","quickbar"),"quickbar needs ten slots");
    auto relog=record(9);relog["state"]=saved.at(0).state;relog["state"]["quickbar"][1]={{"skill","retired"}};
    world.add(9,9,9,relog);check(world.state(9).at("character").at("state").at("quickbar")==slots,"persisted quickbar reloads; retired skills clear");world.remove(9);
    world.action(1,{{"action","skill"},{"skill","boz_ayi"}});check(event(world.state(1),"error","variant"),"Boz Ayı needs the slot-8 flag");
    world.action(1,{{"action","skill_variant"},{"skill","boz_ayi"}});
    world.action(1,{{"action","skill"},{"skill","kanatma"},{"target",2147493651u}});check(event(world.state(1),"error","variant"),"flag swaps Kanatma out of slot 8");
    world.action(1,{{"action","skill"},{"skill","boz_ayi"}});
    const auto cast=world.state(1);uint32_t bear=0;
    for(const auto& e:cast.at("entities"))if(e.value("kind","")=="summon")bear=e.at("id");
    check(bear && entity(cast,bear).at("master")==1 && entity(cast,bear).at("source_uid")==131482 && authority_event(cast,"skill",1,1,1),"Boz Ayı summons the classes.dat bear");
    check(cast.at("character").at("state").at("mana")==after_cast,"summon spends mana");
    // Within two bite intervals (a combat roll may miss): any authoritative bite from the bear on the rat.
    bool bit=false;for(int tick=0;tick<121 && !bit;++tick){ticks(world,1);const auto frame=world.state(1);for(const auto& e:frame.at("events"))bit|=e.value("source",0u)==bear && e.value("target",0u)==2147493651u;}
    check(bit,"the bear bites the nearest hostile");
    ticks(world,20);const int before=own(1).at("mana");world.action(1,{{"action","skill"},{"skill","boz_ayi"},{"target",2147493651u}});
    const auto redirected=world.state(1);
    check(event(redirected,"ack") && redirected.at("character").at("state").at("mana")==before,"recast while it lives redirects it without mana");
    ticks(world,150);const auto fought=world.state(1);
    check(entity(fought,bear).at("hp")<entity(fought,bear).at("max_hp") && fought.at("character").at("state").at("hp")==full_hp,"the rat retaliates against the summon, not the caster");
    ticks(world,400);bool gone=true;const auto later=world.state(1);for(const auto& e:later.at("entities"))gone&=e.at("id")!=bear;
    check(gone,"the summon leaves when its duration ends");
    // PvP skills on Kabadayı players; Zihin Saldırısı at 15 points always imprisons (stun + no skills).
    auto mage=pool(record(3,300));mage["class_id"]=2;mage["state"]["level"]=40;mage["state"]["skills"]={{"meteorit",1},{"zihin_saldirisi",15}};world.add(3,3,3,mage);
    auto foe=pool(record(4,350));foe["state"]["skills"]={{"depar",1},{"zihin_toplama",1}};world.add(4,4,4,foe);
    world.action(3,{{"action","skill"},{"skill","meteorit"},{"target",4}});check(event(world.state(3),"error","target"),"skills need a hostile player");
    world.action(3,{{"action","kabadayi"}});world.action(4,{{"action","kabadayi"}});
    world.action(3,{{"action","skill"},{"skill","meteorit"},{"target",4}});
    bool struck=false;const auto pvp=world.state(3);for(const auto& e:pvp.at("events"))struck|=e.value("source",0u)==3 && e.value("target",0u)==4 && e.at("type")!="skill";
    check(struck,"damage skill strikes a hostile player");ticks(world,80);
    world.action(3,{{"action","skill"},{"skill","zihin_saldirisi"},{"target",4}});
    bool imprisoned=false;const auto foe_state=own(4);for(const auto& b:foe_state.at("buffs"))imprisoned|=b.value("stun",false) && b.value("silence",false) && b.value("harmful",false);
    check(imprisoned,"Zihin Saldırısı rank-15 proc imprisons the player");
    world.action(4,{{"action","skill"},{"skill","depar"},{"target",4}});check(event(world.state(4),"error","state"),"imprisoned player cannot use skills");
    // Yetenekler ✓: pending points for several skills in one request, all-or-nothing within rank 15 and the budget (level 40: 2×40 = 80).
    world.action(3,{{"action","skill_learn"},{"skills",{{"zihin_saldirisi",1}}}});check(event(world.state(3),"error","rank"),"rank 15 is the cap");
    world.action(3,{{"action","skill_learn"},{"skills",{{"buz_oku",15},{"meteorit",14},{"konsantrasyon",15},{"fiziksel_bilgi",15},{"ates_cemberi",15}}}});
    check(event(world.state(3),"error","points") && !own(3).at("skills").contains("buz_oku"),"over-budget confirm commits nothing");
    world.action(3,{{"action","skill_learn"},{"skills",{{"buz_oku",15},{"meteorit",4}}}});
    check(own(3).at("skills").at("buz_oku")==15 && own(3).at("skills").at("meteorit")==5,"confirm invests every pending point");
    ticks(world,601); // Zihin Saldırısı's 30 s prison ends
    // Kanatma's drain (forced roll) takes Kudret every 3 s; it is a Savaşsal effect.
    world.action(1,{{"action","kabadayi"}});world.action(1,{{"action","skill_variant"},{"skill","boz_ayi"}});
    world.destination(1,250,0,1);ticks(world,20); // into the fixture's 200-unit Savaşçı reach
    world.action(1,{{"action","skill"},{"skill","kanatma"},{"target",4}});check(!event(world.state(1),"error"),"Kanatma lands on the Kabadayı foe");
    const int mana_before=own(4).at("mana");ticks(world,60);
    check(own(4).at("mana").get<int>()<mana_before,"Kanatma drain takes the victim's Kudret");
    // Buz Oku crit (forced): 1 s stun beside its 15-point 3 s slow, both Büyüsel; the hit carries the crit flag.
    world.action(3,{{"action","skill"},{"skill","buz_oku"},{"target",4}});
    const auto mage_frame=world.state(3);bool critical=false;for(const auto& e:mage_frame.at("events"))critical|=e.value("source",0u)==3 && e.value("target",0u)==4 && e.value("critical",false);
    const auto chilled=own(4);int stuns=0,slows=0;for(const auto& b:chilled.at("buffs"))if(b.value("id","")=="buz_oku"){stuns+=b.value("stun",false);slows+=b.value("speed",0)==-20;}
    check(critical && stuns==1 && slows==1,"Buz Oku crit stuns and its rank-15 slow lands");
    // Zihin Toplama removes Savaşsal effects only (no 5-point roll at rank 1).
    world.action(4,{{"action","skill"},{"skill","zihin_toplama"},{"target",4}});
    const auto cleansed=own(4);bool kanatma=false,ice=false;for(const auto& b:cleansed.at("buffs")){kanatma|=b.value("id","")=="kanatma";ice|=b.value("id","")=="buz_oku";}
    check(!kanatma && ice,"Zihin Toplama cleanses Savaşsal, not Büyüsel effects");
    check(own(4).at("buffs").at(0).contains("skill_source") && entity(world.state(3),4).at("debuffs").size()==own(4).at("buffs").size(),"effect icons travel with the bearer");
    // Summons attack players hostile to their caster: the bear sent at the Kabadayı foe bites it.
    world.action(1,{{"action","skill_variant"},{"skill","boz_ayi"}});ticks(world,20);
    world.action(1,{{"action","skill"},{"skill","boz_ayi"}});ticks(world,20);
    world.action(1,{{"action","skill"},{"skill","boz_ayi"},{"target",4}});
    uint32_t pet=0;const auto sent=world.state(1);for(const auto& e:sent.at("entities"))if(e.value("kind","")=="summon")pet=e.at("id");
    bool mauled=false;for(int tick=0;tick<121 && !mauled;++tick){ticks(world,1);const auto frame=world.state(4);for(const auto& e:frame.at("events"))mauled|=e.value("source",0u)==pet && e.value("target",0u)==4 && e.value("type","")=="combat";}
    check(pet && mauled,"Boz Ayı bites a Kabadayı player");
}
// Row 31: NONCANONICAL passive regeneration from config "regeneration" (interval ticks, hp, mana), capped at maximum.
void regeneration_rules(ikv::Json config) {
    config["regeneration"]={{"interval",10},{"hp",3},{"mana",2}};
    ikv::World world(config,[](std::span<const ikv::CharacterSave>){});
    auto player=record(1);player["state"]["hp"]=50;player["state"]["mana"]=0;world.add(1,1,1,player);
    const auto own=[&]{return world.state(1).at("character").at("state");};
    const int max_hp=own().at("max_hp");
    ticks(world,9);check(own().at("hp")==50 && own().at("mana")==0,"no regeneration before the interval");
    ticks(world,1);check(own().at("hp")==53 && own().at("mana")==2,"one regeneration step per interval");
    ticks(world,10*std::max(max_hp,own().at("max_mana").get<int>()));check(own().at("hp")==max_hp && own().at("mana")==own().at("max_mana"),"regeneration stops at maximum");
}
// Tılsım riders (row 23, NONCANONICAL): synthetic carried glyphs drive proc chances, slow scaling, Yıldırım chain and electric
// marks, silenced damage, Defansif stance damage/Enerji, heal cuts, Gazap heals and Kutup Rüzgarı's self-stun.
void glyph_riders(ikv::Json config) {
    const uint32_t rat=2147493651u,second_rat=2147493680u;
    for(auto& n:config["npcs"])if(n.at("id")==rat)n["hp"]=100000;
    for(const auto& n:ikv::Json(config["npcs"]))if(n.at("id")==rat){auto copy=n;copy["id"]=second_rat;copy["x"]=850;copy["patrol"]=ikv::Json::array();config["npcs"].push_back(copy);}
    for(const char* skill:{"yildirim","iyilestirme"})config["skills"][skill]["cooldown"]=20; // recast before marks/heal cuts expire
    const auto glyph=[&](const char* id,ikv::Json skills,const char* kind,int value,const char* field=""){
        ikv::Json g={{"skills",skills},{"kind",kind},{"value",value}};if(*field)g["field"]=field;
        config["items"][id]={{"name",std::string("Test Tılsım ")+id},{"stack",1},{"price",1},{"level",1},{"glyph",g}};};
    glyph("g_free",{"sert_vurus"},"proc",100,"free");glyph("g_silence",{"durdurma"},"proc",100,"silence");glyph("g_slow",{"dikkat_dagitma"},"slow",60);
    glyph("g_silenced",{"sert_vurus"},"silenced_damage",100);glyph("g_energy",{"defansif_dovusme"},"armor_energy",450);glyph("g_stance",{"defansif_dovusme"},"stance_damage",100);
    glyph("g_chain",{"yildirim"},"chain",150);glyph("g_cut",{"asit_saldirisi"},"heal_cut",50);glyph("g_wrath",{"iyilestirme"},"wrath_heal",50);glyph("g_kutup",{"kutup_ruzgari"},"self_stun",50);
    glyph("g_buz",{"buz_oku"},"duration",30);config["skills"]["buz_oku"].erase("on_crit"); // no crit stun replacing the slow
    int prefix=0;for(const auto& [id,p]:config.at("prefixes").items())if(p.at("attributes").size()==1 && p.at("attributes")[0][0]==6){prefix=std::stoi(id);break;}
    check(prefix>0,"generated Efsun prefix table has a Maksimum Enerji prefix");
    const double energy=config.at("prefixes").at(std::to_string(prefix)).at("attributes")[0][1];
    ikv::World world(config,[](std::span<const ikv::CharacterSave>){});
    const auto add=[&](uint32_t id,int class_id,int x,ikv::Json skills,std::vector<const char*> glyphs,int hp=1000000){
        auto r=record(int(id),x);r["class_id"]=class_id;r["state"]["level"]=40;r["state"]["mana"]=1000000;r["state"]["hp"]=hp;r["state"]["skills"]=skills; // level 40: Kutup Rüzgarı
        ikv::Json inventory=ikv::Json::array(); // glyphs first: only the four Tılsım-row cells work
        for(size_t i=0;i<glyphs.size();++i)inventory.push_back({{"uid",std::string(31,'d')+"0123456789abcdef"[i]},{"item",glyphs[i]},{"quantity",1}});
        inventory.push_back({{"uid",std::string(32,'e')},{"item","training_blade"},{"quantity",1},{"prefixes",{prefix}}});
        r["state"]["inventory"]=inventory;world.add(id,id,id,r);};
    const auto own=[&](uint32_t id){return world.state(id).at("character").at("state");};
    const auto buffs=[&](uint32_t id){return own(id).at("buffs");}; // by value: range-for keeps the copy alive
    const auto cast=[&](uint32_t id,const char* skill,uint32_t target=0){ikv::Json request={{"action","skill"},{"skill",skill}};if(target)request["target"]=target;world.action(id,request);};
    const auto hit=[&](uint32_t id,uint32_t target){int damage=-1;const auto frame=world.state(id);for(const auto& e:frame.at("events"))if(e.at("type")=="combat" && e.value("source",0u)==id && e.value("target",0u)==target)damage=e.at("damage");return damage;};
    // Defansif Dövüşme stance riders: worn Maksimum Enerji ×(1+4.5) and stance damage ×2 instead of ×0.75.
    add(11,1,-500,{{"defansif_dovusme",1}},{"g_energy","g_stance"});
    const auto before=world.state(11);cast(11,"defansif_dovusme");const auto stance=world.state(11);
    check(stance.at("character").at("state").at("max_hp").get<int>()-before.at("character").at("state").at("max_hp").get<int>()==int(std::lround(energy*5.5))-int(std::lround(energy)),"armor_energy Tılsım adds worn Maksimum Enerji ×4.5 while Defansif is open");
    check(stance.at("stats").at("weapon_damage")[1].get<int>()>=2*before.at("stats").at("weapon_damage")[1].get<int>()-1,"stance_damage Tılsım: Defansif deals ×2 instead of ×0.75");
    // Warrior riders on a Kabadayı foe: Bedelsiz Yetenek +100, Durdurma mind lock +100, silenced damage ×2, Dikkat Dağıtma slow ×1.6.
    add(12,1,100,{{"ofansif_dovus",1},{"sert_vurus",5},{"durdurma",5},{"dikkat_dagitma",1}},{"g_free","g_silence","g_silenced","g_slow"});add(13,1,150,ikv::Json::object(),{});
    for(uint32_t id:{12u,13u})world.action(id,{{"action","kabadayi"}});
    cast(12,"ofansif_dovus");ticks(world,20);const int mana=own(12).at("mana");
    cast(12,"sert_vurus",13);const int plain=hit(12,13);
    check(plain>0 && own(12).at("mana")==mana,"proc Tılsım raises Bedelsiz Yetenek to a certain free cast");
    ticks(world,200);cast(12,"durdurma",13);ticks(world,20);
    bool silenced=false;for(const auto& b:buffs(13))silenced|=b.value("silence",false);
    check(silenced,"proc Tılsım makes Durdurma lock the mind");
    cast(12,"sert_vurus",13);check(hit(12,13)==2*plain,"silenced_damage Tılsım doubles Sert Vuruş on a mind-locked foe");
    ticks(world,20);cast(12,"dikkat_dagitma",13);
    int slow=0;for(const auto& b:buffs(13))if(b.value("id","")=="dikkat_dagitma")slow=b.value("speed",0);
    check(slow==-80,"slow Tılsım scales Dikkat Dağıtma 50 % → 80 %");
    // Only the Tılsım row works (the first 4 Eşyalar cells, 2 red + 2 blue in the UI2 art); slotting one drains Kudret (fandom SSS).
    add(20,1,120,{{"dikkat_dagitma",1}},{"g_free","g_silence","g_silenced","g_cut","g_slow"});world.action(20,{{"action","kabadayi"}});
    ticks(world,600);cast(20,"dikkat_dagitma",13);
    slow=0;for(const auto& b:buffs(13))if(b.value("id","")=="dikkat_dagitma")slow=b.value("speed",0);
    check(slow==-50,"a Tılsım in the fifth bag cell does nothing");
    world.action(20,{{"action","item_move"},{"uid",std::string(31,'d')+"4"},{"to",0}});
    check(own(20).at("mana")==0 && own(20).at("inventory")[0].at("item")=="g_slow","slotting a Tılsım into the row drains Kudret");
    // Asit Saldırısı heal_cut halves the victim's own heals; Gazap Tılsım allows healing at 50 %.
    add(15,3,100,{{"iyilestirme",1},{"gazap",1}},{},5000);add(16,3,100,{{"asit_saldirisi",1}},{"g_cut"});add(17,3,-600,{{"iyilestirme",1},{"gazap",1}},{"g_wrath"},5000);
    for(uint32_t id:{15u,16u})world.action(id,{{"action","kabadayi"}});
    int hp=own(15).at("hp");cast(15,"iyilestirme",15);const int full=own(15).at("hp").get<int>()-hp;
    ticks(world,20);cast(16,"asit_saldirisi",15);hp=own(15).at("hp");cast(15,"iyilestirme",15);
    check(full>0 && own(15).at("hp").get<int>()-hp==full*50/100,"heal_cut Tılsım halves the acid victim's heals");
    ticks(world,20);cast(15,"gazap");ticks(world,20);cast(15,"iyilestirme",15);check(event(world.state(15),"error","wrath"),"Gazap still blocks healing without its Tılsım");
    cast(17,"gazap");ticks(world,20);hp=own(17).at("hp");cast(17,"iyilestirme",17);
    check(own(17).at("hp").get<int>()-hp==full*50/100,"wrath_heal Tılsım heals at 50 % under Gazap");
    // Yıldırım: chain to a second monster near the target; electric marks stack to 3 on a Kabadayı foe.
    add(14,2,700,{{"yildirim",1}},{"g_chain"});
    cast(14,"yildirim",rat);const auto chained=world.state(14);
    check(entity(chained,second_rat).at("hp")<entity(chained,second_rat).at("max_hp"),"chain Tılsım: Yıldırım hits a second monster beside its target");
    world.action(14,{{"action","kabadayi"}});
    for(int n=0;n<4;++n){ticks(world,20);cast(14,"yildirim",13);}
    int marks=0,entries=0;for(const auto& b:buffs(13))if(b.contains("electric")){marks=b.at("electric");++entries;}
    check(entries==1 && marks==3,"Yıldırım electric marks stack to three and refresh");
    // Kutup Rüzgarı freezes its caster too; the Tılsım halves it.
    add(18,2,-900,{{"kutup_ruzgari",1}},{"g_kutup"});cast(18,"kutup_ruzgari");
    bool frozen=false;for(const auto& b:buffs(18))frozen|=b.value("id","")=="kutup_ruzgari" && b.value("stun",false) && b.at("expires")==world.tick()+50;
    check(frozen,"self_stun Tılsım halves Kutup Rüzgarı's self-freeze");
    // Buz Oku 2 "yavaşlatma süresini %%30 arttırır": the 5-puan on_hit slow (3 s) lasts 3.9 s.
    add(19,2,200,{{"buz_oku",5}},{"g_buz"});world.action(19,{{"action","kabadayi"}});cast(19,"buz_oku",13);
    bool longer=false;for(const auto& b:buffs(13))longer|=b.value("id","")=="buz_oku" && b.value("speed",0)<0 && b.at("expires")==world.tick()+78;
    check(longer,"duration Tılsım lengthens Buz Oku's slow 3 s → 3.9 s");
}
// Rows 7/18: quest stage events, collect drops handed over, auto follow-up, talk hand-in at another NPC, GB-Anı re-take,
// timed expiry and the legacy {completed:true} migration into quests_done.
void quest_rules(ikv::Json config) {
    const uint32_t giver=2147493649u, merchant=2147493650u, rat=2147493651u, gb=2147493670u;
    const ikv::Json none={{"xp",10},{"currency",5},{"items",ikv::Json::array()}};
    config["quests"]["c1"]={{"name","Toplama"},{"giver",giver},{"level",1},{"requires","first_hunt"},{"objective",{{"kind","collect"},{"target","training_rat"},{"item","rat_tail"},{"count",1}}},{"complete_text","Sağ ol."},{"reward",none}};
    config["quests"]["c2"]={{"name","Haber"},{"giver",giver},{"turn_in",merchant},{"level",1},{"auto",true},{"requires","c1"},{"objective",{{"kind","talk"}}},{"reward",none}};
    config["quests"]["t1"]={{"name","Acele"},{"giver",giver},{"level",1},{"time_limit",1},{"objective",{{"kind","kill"},{"target","training_rat"},{"count",5}}},{"reward",none}};
    for(auto& n:config["npcs"]){if(n.at("id")==giver)n["quests"]={"first_hunt","c1","t1"};if(n.at("id")==merchant)n["quests"]={"c2"};}
    config["npcs"].push_back({{"id",gb},{"kind","questgiver"},{"auto_quests",true},{"name","GB-ANI"},{"zone","eminonu"},{"x",200},{"y",100},{"hp",100},{"dialogue","GB"},{"quests",ikv::Json::array()},{"shop",ikv::Json::array()}});
    ikv::World world(config,[](std::span<const ikv::CharacterSave>){});
    auto player=record(1,300);player["state"]["quests"]={{"first_hunt",{{"progress",1},{"completed",true}}},{"retired",{{"progress",0},{"completed",false}}}};
    world.add(1,1,1,player);
    const auto own=[&]{return world.state(1).at("character").at("state");};
    const auto stage=[](const ikv::Json& s,const char* quest,const char* name){for(const auto& e:s.at("events"))if(e.at("type")=="quest" && e.at("quest")==quest && e.at("stage")==name)return e;return ikv::Json();};
    auto s=world.state(1);
    check(s.at("character").at("state").at("quests_done")==ikv::Json::array({"first_hunt"}) && s.at("character").at("state").at("quests").empty(),"legacy completed quest migrates; retired ids drop");
    check(s.at("catalogs").at("quests").contains("c1") && !s.at("catalogs").at("quests").contains("c2"),"visible NPC offers join the quest catalog; an untaken auto quest is offered by nobody but GB-Anı");
    world.action(1,{{"action","quest_accept"},{"target",giver},{"quest","first_hunt"}});check(event(world.state(1),"error","quest"),"completed quest cannot be taken again");
    world.action(1,{{"action","quest_accept"},{"target",giver},{"quest","t1"}});check(!stage(world.state(1),"t1","accepted").is_null(),"accepted event");
    ticks(world,40);s=world.state(1);
    check(!stage(s,"t1","expired").is_null() && !s.at("character").at("state").at("quests").contains("t1"),"timed quest expires");
    world.action(1,{{"action","quest_accept"},{"target",giver},{"quest","c1"}});
    world.action(1,{{"action","attack"},{"target",rat}});ticks(world,40);s=world.state(1);
    check(!stage(s,"c1","progress").is_null() && stage(s,"c1","ready").at("count")==1 && !uid(s,"rat_tail").empty(),"collect drop: progress and ready events");
    world.destination(1,300,0,2);ticks(world,8);
    world.action(1,{{"action","quest_complete"},{"target",giver},{"quest","c1"}});s=world.state(1);
    const auto done=stage(s,"c1","completed");
    check(!done.is_null() && done.at("text")=="Sağ ol." && !stage(s,"c2","accepted").is_null(),"completion text and auto follow-up");
    bool tail=false;for(const auto& e:s.at("character").at("state").at("inventory"))tail|=e.at("item")=="rat_tail";
    check(!tail && s.at("character").at("state").at("quests").at("c2").at("progress")==1,"collected items handed over; talk quest ready");
    world.action(1,{{"action","quest_complete"},{"target",giver},{"quest","c2"}});check(event(world.state(1),"error","quest"),"talk quest is handed in at turn_in only");
    world.action(1,{{"action","quest_abandon"},{"quest","c2"}});
    world.action(1,{{"action","quest_accept"},{"target",giver},{"quest","c2"}});check(event(world.state(1),"error","quest"),"auto quest is not re-taken at its giver");
    world.destination(1,250,50,3);ticks(world,8);world.state(1);
    world.action(1,{{"action","interact"},{"target",gb}});
    bool offered=false;s=world.state(1);for(const auto& e:s.at("events"))if(e.at("type")=="dialogue")offered=e.at("quests")==ikv::Json::array({"c2"});
    check(offered,"GB-Anı offers deleted auto quests");
    world.action(1,{{"action","quest_accept"},{"target",gb},{"quest","c2"}});
    world.destination(1,350,0,4);ticks(world,8);
    world.action(1,{{"action","quest_complete"},{"target",merchant},{"quest","c2"}});
    check(own().at("quests_done")==ikv::Json::array({"c1","c2","first_hunt"}),"talk quest completed at turn_in; quests_done stays sorted");
}
// Wave 11 objective kinds and rules: visit (a <loc> spot, once a second), use (Kullan on a prop), OFF-Q level requirement
// (min_level), several prerequisites (requires_also), bag-collect progress with an extra item ("also"), a class-limited quest,
// and rewards handed out as the class/sex variant of one name ("{sınıf}" names the class in the completion line).
void quest_objectives(ikv::Json config) {
    const uint32_t giver=2147493649u, merchant=2147493650u, prop=2147493671u;
    const ikv::Json none={{"xp",10},{"currency",5},{"items",ikv::Json::array()}};
    auto armour=config["items"]["training_blade"];armour["name"]="Test Zırhı";armour["class"]=0;armour["sex"]=1;config["items"]["armour_m"]=armour;
    armour["sex"]=0;config["items"]["armour_f"]=armour;
    auto blade=config["items"]["training_blade"];blade["name"]="Test Hançeri";blade["class"]=1;config["items"]["blade_b"]=blade;
    auto prize=config["items"]["healing_tonic"];prize["name"]="Savaşçı Ödülü";config["items"]["prize_s"]=prize;prize["name"]="Büyücü Ödülü";config["items"]["prize_b"]=prize;
    config["quests"]["v1"]={{"name","Keşif"},{"giver",giver},{"level",1},{"objective",{{"kind","visit"},{"zone","eminonu"},{"x",900},{"y",0},{"radius",150}}},
                            {"complete_text","Al bakalım {sınıf} kardeşim."},{"reward",{{"xp",10},{"currency",0},{"items",{{{"item","armour_m"},{"quantity",1}},{{"item","blade_b"},{"quantity",1}},
                            {{"item","prize_s"},{"quantity",1},{"classes",{1,3}}},{{"item","prize_b"},{"quantity",1},{"classes",{2}}}}}}}};
    config["quests"]["u1"]={{"name","Stoklar"},{"giver",giver},{"level",1},{"objective",{{"kind","use"},{"targets",{prop}},{"count",1}}},{"reward",none}};
    config["quests"]["m1"]={{"name","Seviye"},{"giver",giver},{"level",3},{"min_level",2},{"objective",{{"kind","talk"}}},{"reward",none}};
    config["quests"]["r1"]={{"name","Çifte"},{"giver",giver},{"level",1},{"requires","v1"},{"requires_also",{"u1"}},{"objective",{{"kind","talk"}}},{"reward",none}};
    config["quests"]["b1"]={{"name","Çanta"},{"giver",giver},{"level",1},{"objective",{{"kind","collect"},{"item","healing_tonic"},{"count",5},{"also",{{{"item","training_blade"},{"count",1}}}}}},{"reward",none}};
    config["quests"]["w1"]={{"name","Takas"},{"giver",giver},{"level",1},{"class",2},{"objective",{{"kind","talk"}}},{"reward",none}};
    config["quests"]["l1"]={{"name","Tecrübe"},{"giver",giver},{"level",1},{"objective",{{"kind","talk"}}},{"reward",{{"xp",1500},{"currency",0},{"items",ikv::Json::array()}}}};
    const uint32_t cin1=2147493672u, cin2=2147493673u;
    config["quests"]["u2"]={{"name","Kurtarıcımız"},{"giver",giver},{"level",1},{"objective",{{"kind","use"},{"targets",{cin1}},{"count",2}}},{"reward",none}};
    config["quests"]["u3"]={{"name","Kadim İşçilik"},{"giver",giver},{"level",1},{"objective",{{"kind","use"},{"targets",{prop}},{"count",2},{"cooldown",1}}},{"reward",none}};
    for(auto& n:config["npcs"])if(n.at("id")==giver)n["quests"]={"first_hunt","v1","u1","m1","r1","b1","w1","u2","u3","l1"};
    config["npcs"].push_back({{"id",prop},{"kind","prop"},{"interactable",true},{"name","Stok"},{"zone","eminonu"},{"x",300},{"y",100},{"hp",100},{"dialogue","Stok"},{"quests",ikv::Json::array()},{"shop",ikv::Json::array()}});
    for(const auto& [cin,y]:{std::pair{cin1,50},{cin2,-50}})
        config["npcs"].push_back({{"id",cin},{"kind","prop"},{"interactable",true},{"rescue",true},{"respawn",40},{"source_uid",17096},{"source_class",394},{"name","Biçare Folklor Cini"},{"zone","eminonu"},{"x",300},{"y",y},{"hp",1},{"dialogue","Cin"},{"quests",ikv::Json::array()},{"shop",ikv::Json::array()}});
    ikv::World world(config,[](std::span<const ikv::CharacterSave>){});
    auto player=record(1,300);player["appearance"]={{"sex",0}};player["state"]["currency"]=1000;world.add(1,1,1,player);
    const auto own=[&]{return world.state(1).at("character").at("state");};
    const auto stage=[](const ikv::Json& s,const char* quest,const char* name){for(const auto& e:s.at("events"))if(e.at("type")=="quest" && e.at("quest")==quest && e.at("stage")==name)return e;return ikv::Json();};
    int seq=1; const auto walk=[&](int x){world.destination(1,x,0,uint32_t(seq++));ticks(world,30);};
    // Konuş and the giver's state entity list only quests takeable now: an under-level (min_level), other-class or locked one
    // stays hidden until "Seviye atladıkça, OOK'ler size daha önce yapmadığınız görevleri verebilirler" (dh_rehber l.402).
    const auto offered=[&](const char* q){
        world.action(1,{{"action","interact"},{"target",giver}});const auto f=world.state(1);int seen=0;
        for(const auto& e:f.at("events"))if(e.at("type")=="dialogue" && std::ranges::find(e.at("quests"),ikv::Json(q))!=e.at("quests").end())++seen;
        for(const auto& e:f.at("entities"))if(e.at("id")==giver && std::ranges::find(e.at("quests"),ikv::Json(q))!=e.at("quests").end())++seen;
        return seen;};
    check(offered("v1")==2 && !offered("m1") && !offered("w1") && !offered("r1"),"under-level, other-class and locked quests are not offered");
    world.action(1,{{"action","quest_accept"},{"target",giver},{"quest","m1"}});check(event(world.state(1),"error","quest"),"OFF-Q level requirement (min_level) refuses a lower level");
    world.action(1,{{"action","quest_accept"},{"target",giver},{"quest","w1"}});check(event(world.state(1),"error","quest"),"a class-limited quest refuses other classes");
    world.action(1,{{"action","quest_accept"},{"target",giver},{"quest","r1"}});check(event(world.state(1),"error","quest"),"a quest waits for every prerequisite");
    world.action(1,{{"action","quest_accept"},{"target",giver},{"quest","l1"}});world.action(1,{{"action","quest_complete"},{"target",giver},{"quest","l1"}});
    check(own().at("level")==2 && offered("m1")==2,"reaching min_level offers the quest at once");
    // use: Kullan on the prop
    world.action(1,{{"action","quest_accept"},{"target",giver},{"quest","u1"}});world.state(1);
    world.action(1,{{"action","interact"},{"target",prop}});auto s=world.state(1);
    check(!stage(s,"u1","progress").is_null() && !stage(s,"u1","ready").is_null(),"use objective: Kullan on the prop progresses the quest");
    world.action(1,{{"action","quest_complete"},{"target",giver},{"quest","u1"}});check(!stage(world.state(1),"u1","completed").is_null(),"use quest handed in");
    // count>1 use: each trapped Cin (same classes.dat uid) frees once and leaves; one prop repeats only after the cooldown
    for(const char* q:{"u2","u3"})world.action(1,{{"action","quest_accept"},{"target",giver},{"quest",q}});
    world.state(1);
    const auto progress=[&](const char* q){return own().at("quests").at(q).at("progress").get<int>();};
    const auto seen=[&](uint32_t id){const auto f=world.state(1);for(const auto& e:f.at("entities"))if(e.at("id")==id)return true;return false;};
    world.action(1,{{"action","interact"},{"target",cin1}});check(progress("u2")==1 && !seen(cin1),"rescue: Kullan frees the trapped Cin, which leaves");
    world.action(1,{{"action","interact"},{"target",cin1}});check(progress("u2")==1,"a freed Cin cannot be freed again");
    world.action(1,{{"action","interact"},{"target",cin2}});s=world.state(1);check(progress("u2")==2 && !stage(s,"u2","ready").is_null(),"another Cin of the same family counts");
    ticks(world,45);check(seen(cin1),"a freed Cin returns after its respawn");
    world.action(1,{{"action","interact"},{"target",prop}});world.action(1,{{"action","interact"},{"target",prop}});
    check(progress("u3")==1,"spamming one prop inside its cooldown counts once");
    std::this_thread::sleep_for(std::chrono::milliseconds(2100));
    world.action(1,{{"action","interact"},{"target",prop}});check(progress("u3")==2,"the prop counts again after its cooldown");
    for(const char* q:{"u2","u3"})world.action(1,{{"action","quest_complete"},{"target",giver},{"quest",q}});
    world.state(1);
    // visit: reaching the spot completes the objective within a second; class/sex reward variants
    world.action(1,{{"action","quest_accept"},{"target",giver},{"quest","v1"}});world.state(1);
    walk(600);check(own().at("quests").at("v1").at("progress")==0,"visit: outside the radius nothing happens");
    walk(850);s=world.state(1);check(!stage(s,"v1","ready").is_null() && own().at("quests").at("v1").at("progress")==1,"visit: inside the radius the objective is done");
    walk(300);world.action(1,{{"action","quest_complete"},{"target",giver},{"quest","v1"}});s=world.state(1);
    check(stage(s,"v1","completed").at("text")=="Al bakalım savaşçı kardeşim.","{sınıf} names the character's class");
    check(holds(s,"armour_f") && !holds(s,"armour_m") && !holds(s,"blade_b"),"rewards: the own class/sex variant, never another class's weapon");
    check(holds(s,"prize_s") && !holds(s,"prize_b"),"rewards: a line tagged for other classes (W-REW class page) is skipped");
    // requires_also: both prerequisites done
    world.action(1,{{"action","quest_accept"},{"target",giver},{"quest","r1"}});check(!stage(world.state(1),"r1","accepted").is_null(),"every prerequisite done: accepted");
    // bag collect with an extra item: progress follows the bag, the hand-in takes everything
    world.action(1,{{"action","quest_accept"},{"target",giver},{"quest","b1"}});s=world.state(1); // defaults: 3 tonics, 1 blade
    check(own().at("quests").at("b1").at("progress")==3,"collect: items already carried count at once");
    walk(400);world.action(1,{{"action","buy"},{"target",merchant},{"item","healing_tonic"},{"quantity",2}});s=world.state(1);
    check(own().at("quests").at("b1").at("progress")==4,"collect: the bag counts, capped while an 'also' item is missing");
    world.action(1,{{"action","buy"},{"target",merchant},{"item","training_blade"},{"quantity",1}});s=world.state(1);
    check(!stage(s,"b1","ready").is_null() && own().at("quests").at("b1").at("progress")==5,"collect: ready once every item is carried");
    walk(300);world.action(1,{{"action","quest_complete"},{"target",giver},{"quest","b1"}});s=world.state(1);
    int blades=0;for(const auto& e:s.at("character").at("state").at("inventory"))blades+=e.at("item")=="training_blade";
    check(!stage(s,"b1","completed").is_null() && !holds(s,"healing_tonic") && blades==1,"collect: bag items handed over, the worn blade stays (\"çantanızdayken\")");
}
// NONCANONICAL creation kit (defaults.inventory; generate_content.cpp copies quest 1's gear lines): a new character gets each line
// as its own class/sex variant and wears it, quest 1 hands no second piece while one is carried (a character created before the
// kit still gets it there), and a relog keeps exactly one kit.
void starter_kit(ikv::Json config) {
    const uint32_t giver=2147493649u;
    // Arbitrary synthetic visual metadata (as training_blade's in main), not a canonical armour mapping.
    const ikv::Json armour={{"name","Test Ceketi"},{"stack",1},{"price",10},{"slot","body"},{"level",1},{"armor",5},{"sex",1},{"source_uid",4103},{"source_slot",2}};
    config["items"]["armour_m"]=armour;config["items"]["armour_f"]=armour;config["items"]["armour_f"]["sex"]=0;
    auto blade=config["items"]["training_blade"];blade["class"]=0;config["items"]["blade_s"]=blade;blade["class"]=1;config["items"]["blade_b"]=blade;
    const ikv::Json kit={{{"item","armour_m"},{"quantity",1}},{{"item","blade_s"},{"quantity",1},{"classes",{1}}},{{"item","blade_b"},{"quantity",1},{"classes",{2}}}};
    config["defaults"]["inventory"]={{{"item","healing_tonic"},{"quantity",3}}};for(const auto& line:kit)config["defaults"]["inventory"].push_back(line);
    config["quests"]["k1"]={{"name","Teşkilat'a Katılış"},{"giver",giver},{"level",1},{"objective",{{"kind","talk"}}},{"reward",{{"xp",10},{"currency",0},{"items",kit}}}};
    for(auto& n:config["npcs"])if(n.at("id")==giver)n["quests"].push_back("k1");
    std::vector<ikv::CharacterSave> saved;
    ikv::World world(config,[&](std::span<const ikv::CharacterSave> states){saved.assign(states.begin(),states.end());});
    auto fresh=record(1,300);fresh["class_id"]=2;fresh["appearance"]={{"sex",0}};fresh["state"]={{"x",300},{"y",0}};world.add(1,1,1,fresh);
    auto old=record(2,300);old["state"]["inventory"]=ikv::Json::array();world.add(2,2,2,old); // created before the kit: an empty bag
    const auto count=[&](uint32_t id,const char* item){int n=0;const auto s=world.state(id);for(const auto& e:s.at("character").at("state").at("inventory"))if(e.at("item")==item)n+=e.at("quantity").get<int>();return n;};
    const auto wears=[&](uint32_t id,const char* slot,const char* item){const auto s=world.state(id).at("character").at("state");bool on=false;
        for(const auto& e:s.at("inventory"))on=on||(e.at("item")==item && s.at("equipment").value(slot,ikv::Json())==e.at("uid"));return on;};
    check(count(1,"healing_tonic")==3 && count(1,"armour_f")==1 && count(1,"blade_b")==1 && !count(1,"armour_m") && !count(1,"blade_s"),
          "a new character gets the kit as its own class/sex variant");
    check(wears(1,"body","armour_f") && wears(1,"weapon","blade_b") && world.state(1).at("character").at("equipment").size()==2,"the kit is worn and visible");
    for(uint32_t p:{1u,2u}){world.action(p,{{"action","quest_accept"},{"target",giver},{"quest","k1"}});world.action(p,{{"action","quest_complete"},{"target",giver},{"quest","k1"}});}
    check(count(1,"armour_f")==1 && count(1,"blade_b")==1,"quest 1 hands no second kit piece while one is carried");
    check(count(2,"armour_m")==1 && count(2,"blade_s")==1 && !count(2,"blade_b"),"a character without the kit still gets quest 1's gear");
    world.remove(1);check(saved.size()==1,"logout saves");auto again=fresh;again["state"]=saved[0].state;world.add(1,1,1,again);
    check(count(1,"armour_f")==1 && count(1,"blade_b")==1 && count(1,"healing_tonic")==3 && wears(1,"body","armour_f"),"a relog keeps exactly one kit");
}
// Server-side bots (NONCANONICAL): no player slot, only validated client actions (range, cooldowns, quest givers), deferred
// persistence of bot-only progress, companion invitation/heal/door follow/assist/dismiss/leader logout, and a tick budget.
void bot_rules(ikv::Json config) {
    const uint32_t rat=2147493651u,door=2147493652u,yard_rat=2147493660u; // questgiver stands at (200,0)
    for(const auto& n:ikv::Json(config["npcs"]))if(n.at("id")==rat){auto copy=n;copy["id"]=yard_rat;copy["zone"]="old_town";copy["x"]=600;copy["patrol"]={{600,0},{650,0}};config["npcs"].push_back(copy);}
    std::vector<ikv::CharacterSave> saved;
    ikv::World world(config,[&](std::span<const ikv::CharacterSave> states){saved.insert(saved.end(),states.begin(),states.end());});
    const auto bot=[&](int id,int class_id,int x,const char* behaviour) {
        auto fresh=world.bot_character(1);fresh["state"]["x"]=x;fresh["class_id"]=class_id;fresh["state"].erase("hp");
        return world.add_bot(id,id,{{"name",fresh.at("name")},{"class_id",class_id},{"appearance",fresh.at("appearance")},{"state",fresh.at("state")}},{{"behaviour",behaviour}});
    };
    const auto row=[&](uint32_t id){for(const auto& r:world.bots())if(r.at("id")==id)return r;throw std::runtime_error("bot row missing");};
    const auto saves_of=[&](int64_t character){return std::count_if(saved.begin(),saved.end(),[&](const ikv::CharacterSave& s){return s.character_id==character;});};
    // Bots never take one of the 32 player slots.
    for(int id=1;id<=32;++id)world.add(id,id,id,record(id,1300));
    const auto hunter=bot(900,1,0,"hunt");
    bool full=false;try{world.add(33,33,33,record(33));}catch(const std::invalid_argument&){full=true;}
    check(full && hunter>=0x70000000u && hunter<=INT32_MAX,"bots are admitted beside 32 players, a 33rd player is refused");
    for(int id=2;id<=32;++id)world.remove(id);
    check(entity(world.state(1),hunter).at("kind")=="player","bots appear to clients as players");
    // Hunt: the bot walks to the quest giver, hunts, loots and hands in, through handle(): every hit within reach and cooldown.
    const int reach=config.at("combat_range").get<int>()+config.at("movement_step").get<int>();
    std::vector<uint32_t> swings,casts;bool accepted_near=true;
    for(int t=0;t<1200;++t) {
        const bool before=world.state(hunter).at("character").at("state").at("quests").contains("first_hunt");
        world.advance();
        const auto own=world.state(hunter).at("character").at("state");
        if(!before && own.at("quests").contains("first_hunt") && std::abs(own.at("x").get<int>()-200)>config.at("interaction_range").get<int>())accepted_near=false;
        const auto seen=world.state(1);
        for(const auto& e:seen.at("events"))if(e.value("source",0u)==hunter && (e.at("type")=="combat" || (e.at("type")=="skill" && e.contains("damage")))) {
            const auto& b=entity(seen,hunter);const auto target=e.at("target").get<uint32_t>();
            for(const auto& v:seen.at("entities"))if(v.at("id")==target)check(std::abs(v.at("x").get<int>()-b.at("x").get<int>())<=reach,"bot hits only within weapon/skill reach");
            (e.at("type")=="combat"?swings:casts).push_back(world.tick());
        }
    }
    for(size_t i=1;i<swings.size();++i)check(swings[i]-swings[i-1]>=10,"bot auto-attack keeps the weapon attack cooldown");
    for(size_t i=1;i<casts.size();++i)check(casts[i]-casts[i-1]>=config.at("skill_global_cooldown").get<uint32_t>(),"bot skills keep the global cooldown");
    auto own=world.state(hunter).at("character").at("state");
    check(accepted_near && !swings.empty(),"bot accepts quests only at the giver and fights");
    const auto done=own.at("quests_done");
    check(std::find(done.begin(),done.end(),ikv::Json("first_hunt"))!=done.end() && own.at("xp").get<int>()+own.at("level").get<int>()>1,"bot completes the kill quest at its giver");
    check(holds(world.state(hunter),"rat_tail"),"bot takes its own Ganimet");
    check(saves_of(900)==0,"bot-only progress is not written per action");
    world.remove(hunter);
    check(saves_of(900)==1 && saved.back().state.at("quests_done")==done && world.released_bots()==std::vector<std::pair<int64_t,int64_t>>{{900,900}},
          "a removed bot persists its loot and quest progress and releases its lease");
    // Companion: accepts a player's invitation, heals the hurt leader, follows through a door, assists, leaves on /dismiss and logout.
    world.remove(1);auto hurt=record(1,-700);hurt["state"]["hp"]=100;world.add(1,1,1,hurt);
    const auto mate=bot(901,3,-600,"companion");
    world.action(1,{{"action","group_invite"},{"target",mate}});ticks(world,120); // accepted after a human reaction delay
    check(row(mate).at("leader")==1,"companion accepts a player's group invitation");
    bool healed=false;for(int t=0;t<40 && !healed;++t){world.advance();const auto seen=world.state(1);for(const auto& e:seen.at("events"))healed=healed||(e.at("type")=="skill" && e.at("source")==mate && e.at("target")==1);}
    check(healed,"Şifacı companion heals its hurt leader");
    world.action(1,{{"action","chat"},{"channel","private"},{"to",mate},{"text","naber, nasılsın?"}});
    bool answered=false;for(int t=0;t<200 && !answered;++t){world.advance();const auto seen=world.state(1);for(const auto& e:seen.at("events"))answered=answered||(e.at("type")=="chat" && e.at("from")==mate && e.at("channel")=="private");}
    check(answered,"a bot answers a player's whisper after a reaction delay");
    world.action(1,{{"action","interact"},{"target",door}});ticks(world,25);
    check(world.state(1).at("zone")=="old_town","leader uses the door");
    ticks(world,200);
    check(row(mate).at("zone")=="old_town","companion follows the leader through the same door");
    world.action(1,{{"action","attack"},{"target",yard_rat}});
    bool assisted=false;for(int t=0;t<200 && !assisted;++t){world.advance();const auto seen=world.state(1);for(const auto& e:seen.at("events"))assisted=assisted||(e.value("source",0u)==mate && e.value("target",0u)==yard_rat);}
    check(assisted,"companion assists the leader's target");
    world.action(1,{{"action","chat"},{"channel","group"},{"text","/dismiss"}});ticks(world,40);
    check(row(mate).at("leader")==0,"/dismiss releases the companion");
    ticks(world,20);world.action(1,{{"action","group_invite"},{"target",mate}});ticks(world,120);
    check(row(mate).at("leader")==1,"a released companion can be invited again");
    world.remove(1);ticks(world,40);
    check(row(mate).at("leader")==0,"leader logout releases the companion");
    // Budget: 200 bots at 20 Hz (fixture zones; ikv-server status tick_ms measures original support grids).
    for(int n=0;n<200;++n)bot(1000+n,1+n%3,-400+n*4,n%3==0?"hunt":n%3==1?"town":"companion");
    const auto begin=std::chrono::steady_clock::now();ticks(world,100);
    const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count()/100;
    std::cout<<"bots: 201 bots "<<ms<<" ms/tick\n";
    check(ms<50,"201 bots stay inside the 50 ms tick");
    world.add(2,2,2,record(2));ticks(world,1);
    const auto shown=world.snapshot(2);const auto crowd=world.state(2);std::set<uint32_t> listed{2u},sent;
    for(const auto& e:crowd.at("entities"))if(e.at("kind")=="player")listed.insert(e.at("id").get<uint32_t>());
    for(const auto& p:shown)sent.insert(p.id);
    check(shown.size()==32 && listed==sent,"a bot crowd keeps the snapshot at 32 players, equal to the state's player entities");
}
// Bot quarry fairness (mod-playerbots GrindTargetValue): a hunting bot never takes a monster a player is attacking or has hit,
// even when it is the only monster in reach; it hunts the same monster once the player is gone.
void bot_fairness(ikv::Json config) {
    const uint32_t rat=2147493651u;
    for(auto& n:config["npcs"])if(n.at("id")==rat){n["hp"]=100000;n["patrol"]=ikv::Json::array();} // outlasts the test: no respawn gap
    ikv::World world(config,[](std::span<const ikv::CharacterSave>){});
    auto player=record(1,300);player["state"]["level"]=1;world.add(1,1,1,player);
    auto fresh=world.bot_character(1);fresh["state"]["x"]=350;
    const auto hunter=world.add_bot(902,902,fresh,{{"behaviour","hunt"}});
    world.action(1,{{"action","attack"},{"target",rat}});
    bool took=false;
    for(int t=0;t<600;++t){
        world.advance();
        const auto seen=world.state(1);for(const auto& e:seen.at("events"))took=took||(e.value("source",0u)==hunter && e.value("target",0u)==rat);
    }
    check(!took,"a bot never strikes a monster a player is attacking");
    world.remove(1);
    for(int t=0;t<1200 && !took;++t){world.advance();for(const auto& r:world.bots())took=took||r.at("state")=="fighting";}
    check(took,"the bot hunts that monster once no player is on it");
}
// World-level tap guarantee (not the brain): a bot that clicked a monster first never lands a hit once a player clicks it before
// the bot's first swing, and a bot's later click on it is refused (bot_rules covers a companion assisting its leader's target).
void bot_claim(ikv::Json config) {
    const uint32_t rat=2147493651u;int rx=0,ry=0;
    for(auto& n:config["npcs"])if(n.at("id")==rat){n["hp"]=100000;n["patrol"]=ikv::Json::array();rx=n.at("x");ry=n.at("y");}
    ikv::World world(config,[](std::span<const ikv::CharacterSave>){});
    auto player=record(1,300);player["state"]["level"]=1;world.add(1,1,1,player);
    auto fresh=world.bot_character(1);fresh["state"]["x"]=rx-1200;fresh["state"]["y"]=ry;
    const auto bot=world.add_bot(904,904,fresh,{{"behaviour","town"}});
    const auto state=[&]{for(const auto& r:world.bots())if(r.at("id")==bot)return r.at("state").get<std::string>();return std::string();};
    world.action(bot,{{"action","attack"},{"target",rat}});world.advance();
    check(state()=="fighting","the bot engages the unclaimed monster first");
    world.action(1,{{"action","attack"},{"target",rat}}); // the player clicks before the bot's first hit
    bool hit=false;
    for(int t=0;t<400;++t){world.advance();const auto view=world.state(1);for(const auto& e:view.at("events"))hit=hit||(e.value("source",0u)==bot && e.value("target",0u)==rat);}
    check(!hit && state()!="fighting","a bot that clicked first never hits a monster a player claimed before its first hit");
    world.action(bot,{{"action","attack"},{"target",rat}});world.advance();
    check(state()!="fighting","a bot's click on a player-claimed monster is refused");
}
// Bots spawned together enter at distinct supported spots a straight walk from the zone spawn reaches, never stacked on the spawn.
void bot_spawn_spread(const ikv::Json& mechanics) {
    // SYNTHETIC NOT ORIGINAL: a walkable strip through the spawn (|cell y|<=6) and an unconnected walkable island (cell y 8..10).
    auto grid=std::make_shared<ikv::Zone>();grid->width=grid->height=2048;grid->source="SYNTHETIC NOT ORIGINAL";grid->cell_size=1.5f;
    grid->cells.assign(2048u*2048u,0);grid->heights.assign(grid->cells.size(),0.0f);
    for(int y=-6;y<=10;++y)if(y!=7)for(int x=-12;x<=12;++x)grid->cells[size_t(y+1024)*2048+size_t(x+1024)]=255;
    auto config=mechanics;config["aoi"]=60000; // town wander radius aoi/4 = 15000: reaches the island
    config["zones"]={{"eminonu",{{"source_registry_id",0},{"bounds",{-1536000,-1536000,1534500,1534500}},{"spawn",{0,0}},{"collision",ikv::Json::array()}}}};
    config["npcs"]=ikv::Json::array();config["quests"]=ikv::Json::object();config["zones"]["old_town"]=mechanics.at("zones").at("old_town");
    ikv::World world(config,[](std::span<const ikv::CharacterSave>){});world.set_source_zones({{"eminonu",grid}});
    std::set<std::pair<int,int>> spots;bool reachable=true;
    for(int id=1;id<=12;++id)world.add_bot(id,id,world.bot_character(1),{{"behaviour","town"},{"zone","eminonu"}});
    for(const auto& r:world.bots()) {
        const int x=r.at("x"),y=r.at("y");
        spots.insert({x,y});reachable=reachable&&std::abs(y)<=9750; // cell 7 (y 9750..11250) separates the island
    }
    check(spots.size()==12 && !spots.contains({0,0}),"bots spawned together are admitted at distinct spots, none on the zone spawn");
    check(reachable,"bot spawn spots lie on support walkable straight from the zone spawn");
}
// Group zones and Er Meydanı (original guide rules; lockout, points, stakes and Şef skills are NONCANONICAL): Şef area cast, certain drop
// and copy lockout, copy reset, group-zone safety, banner shield/score/resurrection, /kpsavas stakes paid to the winner, the /savas
// lot prize, guide Şan and Karizma.
void activity_rules(const ikv::Json& mechanics) {
    // SYNTHETIC NOT ORIGINAL support grid: one walkable strip for every zone.
    auto grid=std::make_shared<ikv::Zone>();grid->width=grid->height=2048;grid->source="SYNTHETIC NOT ORIGINAL";grid->cell_size=1.5f;
    grid->cells.assign(2048u*2048u,0);grid->heights.assign(grid->cells.size(),0.0f);
    for(int y=-2;y<=2;++y)for(int x=-12;x<=12;++x)grid->cells[size_t(y+1024)*2048+size_t(x+1024)]=255;
    auto config=mechanics;const ikv::Json bounds={-1536000,-1536000,1534500,1534500};
    const auto zone=[&](int registry,const ikv::Json& instances){ikv::Json z={{"source_registry_id",registry},{"bounds",bounds},{"spawn",{0,0}},{"collision",ikv::Json::array()}};if(!instances.is_null())z["instances"]=instances;return z;};
    config["zones"]={{"eminonu",zone(0,nullptr)},{"migrat",zone(36,{27})},{"savas",zone(6,{7})}};
    config["social"]["safe_zones"]={"migrat"};config["quests"]=ikv::Json::object();
    config["activities"]={{"lockout",1800},{"arena",{{"zone","savas"},{"duration",400},{"kill_points",1},{"banner_points",5},{"guardian_points",3},{"stake",100},{"homes",{{"1","eminonu"},{"2","eminonu"}}},
                                                     {"capture_ticks",40},{"areas",{{{"name","Meydan"},{"x",0},{"y",0},{"radius",500},{"power",50}}}}}}};
    const auto rat=mechanics.at("npcs")[2];
    const uint32_t in=2147493701u,out=2147493702u,chief=2147493700u,arz=2147493703u,lodos=2147493704u,copy=65536u;
    auto boss=rat;boss.update({{"id",chief},{"name","Şef"},{"zone","migrat"},{"x",7500},{"y",0},{"hp",40},{"damage",1},{"aggro",1000},{"respawn",72000},{"patrol",ikv::Json::array()},
                               {"drops",{"training_blade"}},{"loot",ikv::Json::array()},{"boss",true},
                               {"skills",{{{"group",13},{"on","cast"},{"skill","supurme_vurusu"},{"damage",7},{"radius",3000},{"cooldown",20}}}}});
    const auto door=[](uint32_t id,const char* where,int x,const char* to,int ax){return ikv::Json{{"id",id},{"kind","door"},{"name","Kapı"},{"zone",where},{"x",x},{"y",0},{"hp",100},{"dialogue","Kapı"},
                                                                                                 {"quests",ikv::Json::array()},{"shop",ikv::Json::array()},{"door",{{"to",to},{"arrive",{ax,0}}}}};};
    const auto banner=[&](uint32_t id,int clan,int x){auto b=rat;b.update({{"id",id},{"name",clan==1?"Arz Sancağı 1":"Lodos Sancağı 1"},{"zone","savas"},{"x",x},{"y",0},{"hp",30},{"banner",true},{"clan",clan},
                                                                         {"aggro",1000},{"patrol",ikv::Json::array()},{"loot",ikv::Json::array()},{"drops",{"training_blade"}}});return b;};
    auto keeper=rat;keeper.update({{"id",lodos+1},{"kind","questgiver"},{"dialogue","Sancağı koruyorum."},{"name","Lodos Muhafızı"},{"zone","savas"},{"x",1800},{"y",0},{"hp",30},{"damage",1},{"clan",2},
                                   {"aggro",300},{"patrol",ikv::Json::array()},{"loot",ikv::Json::array()}}); // an Er Meydanı guardian (Klan OOK guard)
    config["npcs"]={door(in,"eminonu",15000,"migrat",6000),door(out,"migrat",5600,"eminonu",14000),boss,banner(arz,1,-900),banner(lodos,2,900),keeper};
    ikv::World world(config,[](std::span<const ikv::CharacterSave>){});world.set_source_zones({{"eminonu",grid},{"migrat",grid},{"savas",grid}});
    const auto own=[&](uint32_t p){return world.state(p).at("character").at("state");};
    const auto player=[&](uint32_t p,int x,int clan,int64_t clan_currency=0,int hp=248){auto r=record(int(p),x);if(clan)r["state"]["clan"]=clan;r["state"]["clan_currency"]=clan_currency;r["state"]["hp"]=hp;world.add(p,p,p,r);};
    const auto until=[&](const auto& done,const char* message){for(int t=0;t<800 && !done();++t)world.advance();check(done(),message);};
    const auto hp_of=[&](uint32_t viewer,uint32_t id)->int{const auto frame=world.state(viewer);for(const auto& e:frame.at("entities"))if(e.at("id")==id)return e.at("hp");return -1;};
    const auto chat=[&](uint32_t p,const char* text){ticks(world,11);world.action(p,{{"action","chat"},{"channel","local"},{"text",text}});};
    // Group zone: a group gets its own copy; the Şef's area cast reaches the whole group, its kill drops an Efsun-capable item and locks
    // the zone for the credited members; the emptied copy resets for the next group.
    player(1,14900,1);player(2,14900,2);player(3,14900,0);
    world.action(1,{{"action","group_invite"},{"target",2}});world.action(2,{{"action","group_accept"}});
    for(uint32_t p=1;p<=3;++p)world.action(p,{{"action","interact"},{"target",in}});
    check(own(1).at("zone")=="migrat1" && own(2).at("zone")=="migrat1" && own(3).at("zone")=="migrat","group copy and shared group zone");
    world.action(1,{{"action","attack"},{"target",2}});
    check(event(world.state(1),"error","target"),"no PvP between enemy clans inside a group zone (guide)");
    world.action(1,{{"action","attack"},{"target",chief+copy}});
    until([&]{return hp_of(1,chief+copy)==0;},"group kills the Şef");
    check(own(2).at("hp").get<int>()<248,"the Şef's area cast hits a groupmate it is not fighting");
    check(own(1).at("lockouts").contains("migrat") && own(2).at("lockouts").contains("migrat") && !own(3).contains("lockouts"),"Şef kill credit locks the group zone");
    bool dropped=false;
    for(uint32_t p:{1u,2u}){world.action(p,{{"action","loot"},{"target",chief+copy}});ticks(world,30);
        const auto frame=world.state(p);for(const auto& e:frame.at("events"))if(e.at("type")=="loot_open")for(const auto& slot:e.at("items"))dropped=dropped||(slot.is_object() && slot.value("item","")=="training_blade");}
    check(dropped,"a Şef always drops an item from its table");
    check(hp_of(3,chief)==40,"the shared zone's Şef is untouched");
    for(uint32_t p:{1u,2u})world.action(p,{{"action","interact"},{"target",out+copy}});
    ticks(world,30);check(own(1).at("zone")=="eminonu" && own(2).at("zone")=="eminonu","group leaves the copy");
    world.action(1,{{"action","interact"},{"target",in}});ticks(world,30);
    check(event(world.state(1),"error","lockout") && own(1).at("zone")=="eminonu","locked-out character cannot re-enter the group zone");
    player(4,14900,0);player(5,14900,0);world.action(4,{{"action","group_invite"},{"target",5}});world.action(5,{{"action","group_accept"}});
    world.action(4,{{"action","interact"},{"target",in}});
    check(own(4).at("zone")=="migrat1" && hp_of(4,chief+copy)==40,"the emptied copy reset: the next group meets a living Şef");
    // Wave 15 connected party: invite/accept across zones; "party" carries every other member's status wherever it is; leaving clears it.
    const auto party_row=[&](uint32_t viewer,uint32_t id)->ikv::Json{const auto frame=world.state(viewer);for(const auto& m:frame.at("party"))if(m.at("id")==id)return m;return nullptr;};
    world.action(4,{{"action","group_invite"},{"target",3}});world.action(3,{{"action","group_accept"}}); // leader in migrat1, invitee in migrat
    check(party_row(4,3).value("zone","")=="migrat" && party_row(4,3).at("hp")==own(3).at("hp") && party_row(4,3).at("alive")==true &&
          party_row(4,5).value("zone","")=="eminonu" && party_row(4,5).at("source_registry_id")==0 && party_row(3,4).value("zone","")=="migrat1","cross-zone invite and party status");
    world.action(3,{{"action","group_leave"}});
    check(party_row(4,3).is_null() && world.state(3).at("party").empty() && party_row(4,5).value("zone","")=="eminonu","leaving clears the party row");
    // Er Meydanı: /kpsavas stake prompt and stakes, kind separation, capture areas, banners (own-clan shield, score, resurrection),
    // guardians, kill score, payout, headquarters.
    player(6,0,1,500);player(7,0,2,300,5);player(8,0,0);
    chat(8,"/savas 2");check(event(world.state(8),"error","clan") && own(8).at("zone")=="eminonu","Er Meydanı needs a clan");
    chat(6,"/kpsavas 2 200");
    bool prompted=false;{const auto f=world.state(6);for(const auto& e:f.at("events"))prompted|=e.at("type")=="arena_stake" && e.at("code")==2 && e.at("stake")==200;}
    check(prompted && own(6).at("zone")=="eminonu" && own(6).at("clan_currency")==500,"/kpsavas asks to confirm the stake before entering");
    world.action(6,{{"action","arena_join"},{"code",2},{"stake",200}});
    check(own(6).at("zone")=="savas1" && own(6).at("clan_currency")==300 && own(6).at("x")==-900,"the confirmed stake is taken and the player arrives at the own banner");
    world.action(7,{{"action","arena_join"},{"code",2},{"stake",0}});check(event(world.state(7),"error","arena"),"a running Klan Parası arena refuses /savas entrants");
    world.action(7,{{"action","arena_join"},{"code",2},{"stake",100}});check(own(7).at("zone")=="savas1" && own(7).at("clan_currency")==200 && own(7).at("x")==900,"second clan joins with its stake");
    world.destination(6,0,0,1);
    until([&]{return world.state(6).at("stats").value("arena_power",0)==50;},"a clan alone in an area takes it and its skills gain the area's power");
    check(!world.state(7).at("stats").contains("arena_power"),"the other clan gets no area bonus");
    world.action(6,{{"action","attack"},{"target",arz+copy}});check(event(world.state(6),"error","target"),"a clan cannot strike its own banner");
    world.action(6,{{"action","attack"},{"target",7}});
    until([&]{return own(7).at("hp")==0;},"enemy clan player falls in the arena");
    check(own(6).at("san")==1 && own(6).at("clan_currency")==301 && own(7).at("karizma")==80,"guide Şan/Klan Parası for the killer, -20% Karizma for the victim");
    ticks(world,25);world.action(7,{{"action","resurrect"}});check(own(7).at("x")==900 && own(7).at("hp")==own(7).at("max_hp"),"resurrection at the clan's standing banner");
    world.action(6,{{"action","attack"},{"target",lodos+1+copy}});
    bool guarded=false;for(int t=0;t<800 && !guarded;++t){world.advance();const auto f=world.state(6);for(const auto& e:f.at("events"))guarded|=e.at("type")=="arena" && e.at("text").get<std::string>().find("muhafız alt etti")!=std::string::npos &&
                                                                                                                          e.at("text").get<std::string>().find("Arzın Çocukları 4 - Lodos 0")!=std::string::npos;}
    check(guarded,"felling an enemy guardian scores guardian_points (guide: muhafızları öldürmek puan kazandırır)");
    world.action(6,{{"action","attack"},{"target",lodos+copy}});
    until([&]{return hp_of(6,lodos+copy)==0;},"banner falls");
    until([&]{return own(6).at("zone")=="eminonu";},"the match ends and sends everyone to headquarters");
    check(own(6).at("clan_currency")==301+300 && own(7).at("clan_currency")==200 && own(7).at("zone")=="eminonu","winners take their stake back plus the losers' stakes");
    chat(6,"/savas 1");check(own(6).at("zone")=="savas","/savas 1 is Er Meydanı 1");
    world.action(6,{{"action","attack"},{"target",lodos}});until([&]{return own(6).at("zone")=="eminonu";},"single-clan match ends");
    int blades=0;const auto bag=own(6);for(const auto& e:bag.at("inventory"))blades+=e.at("item")=="training_blade";
    check(blades==2,"the winning clan's lot draws an item prize");
}
// Şef skills and auto_retaliate (both NONCANONICAL): a Şef's cast lays its skill's effect with the NPC as caster, a bite rider
// rides a bite that drew blood, the damage over time it laid ticks from the living Şef, and an idle victim answers a blow only
// when auto_retaliate is on.
void boss_rules(ikv::Json config, bool retaliate) {
    auto grid=std::make_shared<ikv::Zone>();grid->width=grid->height=2048;grid->source="SYNTHETIC NOT ORIGINAL";grid->cell_size=1.5f;
    grid->cells.assign(2048u*2048u,0);grid->heights.assign(grid->cells.size(),0.0f);
    for(int y=-2;y<=2;++y)for(int x=-12;x<=12;++x)grid->cells[size_t(y+1024)*2048+size_t(x+1024)]=255;
    const auto strip=[](int registry){return ikv::Json{{"source_registry_id",registry},{"bounds",{-1536000,-1536000,1534500,1534500}},{"spawn",{0,0}},{"collision",ikv::Json::array()}};};
    config["zones"]={{"eminonu",strip(0)},{"old_town",strip(1)}}; // old_town: the fixture's arena zone
    config["quests"]=ikv::Json::object();config["auto_retaliate"]=retaliate;
    const uint32_t chief=2147493700u;
    auto boss=config.at("npcs")[2];
    boss.update({{"id",chief},{"name","Şef"},{"x",600},{"y",0},{"hp",100000},{"damage",1},{"aggro",3000},{"leash",30000},{"patrol",ikv::Json::array()},{"boss",true},{"drops",ikv::Json::array()},
                 {"skills",{{{"group",7},{"on","cast"},{"skill","durdurma"},{"damage",3},{"radius",0},{"cooldown",60},{"duration",40}},
                            {{"group",1},{"on","hit"},{"skill","zehirleme"},{"damage",40},{"radius",0},{"cooldown",20},{"duration",200}}}}});
    config["npcs"]={boss};
    ikv::World world(config,[](std::span<const ikv::CharacterSave>){});world.set_source_zones({{"eminonu",grid},{"old_town",grid}});
    auto player=record(1);player["state"]["hp"]=248;world.add(1,1,1,player);
    bool cast=false,stunned=false,poisoned=false,ticked=false,answered=false;
    for(int t=0;t<200;++t){
        world.advance();const auto frame=world.state(1);
        for(const auto& e:frame.at("events")){
            cast=cast||(e.at("type")=="skill" && e.at("source")==chief && e.at("skill")=="durdurma" && e.at("target")==1);
            ticked=ticked||(e.at("type")=="combat" && e.at("source")==chief && e.value("damage",0)>=5); // bites 1, casts 3, poison 40/4
            answered=answered||(e.at("type")=="combat" && e.at("source")==1 && e.at("target")==chief);
        }
        for(const auto& b:frame.at("character").at("state").at("buffs")){
            stunned=stunned||(b.value("stun",false) && b.at("source")==chief);
            poisoned=poisoned||(b.contains("dot") && b.at("source")==chief && b.at("id")=="zehirleme");
        }
    }
    check(cast && stunned,"a Şef's cast lays its skill's stun with the Şef as caster");
    check(poisoned,"a Şef's bite rider poisons the bitten player");
    check(ticked,"poison laid by a living Şef ticks on the player");
    check(answered==retaliate,retaliate?"auto_retaliate: the idle victim attacks back":"without auto_retaliate a struck player does not attack back");
}
// classes.dat idlewalktend: a resting NPC strolls near its spawn, past players beside it, but stands a minute after serving one; 0 stands.
void idle_walk_rules(ikv::Json config) {
    const uint32_t giver=2147493649u; // fixture questgiver at (200,0)
    const auto stroll=[&](int tend,bool talk){
        auto c=config;c["npcs"][0]["idlewalktend"]=tend;c["npcs"][0]["walk_speed"]=300;
        ikv::World world(c,[](std::span<const ikv::CharacterSave>){});auto r=record(1,200);r["state"]["y"]=100;world.add(1,1,1,r); // within interaction_range
        if(talk){world.action(1,{{"action","interact"},{"target",giver}});check(!event(world.state(1),"error"),"talk to the idle walker");}
        bool moved=false;
        for(int t=0;t<500;++t){world.advance();const auto frame=world.state(1);for(const auto& e:frame.at("entities"))if(e.at("id")==giver){
            const int64_t dx=e.at("x").get<int>()-200,dy=e.at("y").get<int>();check(dx*dx+dy*dy<=12000LL*12000,"an idle walk stays within 12 units of the spawn");moved=moved||dx||dy;}}
        return moved;};
    check(stroll(100,false),"idlewalktend 100 walks off even beside a player");
    check(!stroll(100,true),"an idle walker stands for the player it served");
    check(!stroll(0,false),"idlewalktend 0 stands");
}
// Gaps #3b/#8/#9/#10/#14 (analysis/porting-wave11/Rules): classes.dat monster combat numbers meet the Karakter Bilgisi at
// formulas.monster_unit, 2 skill points per level, the free yetenek/direnç reset at level 5, ×10 regeneration while sitting,
// the level-based death fee (Plus pays a third) and resurrection at the nearest Ruh Taşı arrival.
void progression_rules(ikv::Json config) {
    const uint32_t rat=2147493651u;
    {   // A bare-handed level-1 Savaşçı (Saldırı/Savunma 22) against the original Fare (classes.dat 16001: attack 60, defense 50,
        // dmgmin-dmgmax 2-4, criticalchance 3 x2, attackrate 1200 ms) for two minutes.
        auto c=config;auto& r=c["npcs"][2];r["x"]=100;r["patrol"]=ikv::Json::array();r["hp"]=100000;r["attack"]=60.0;r["defense"]=50.0;
        r["damage_range"]={2.0,4.0};r["crit"]={3.0,2.0};r["attack_cooldown"]=24;
        ikv::World world(c,[](std::span<const ikv::CharacterSave>){});world.add(1,1,1,record(1));
        world.action(1,{{"action","attack"},{"target",rat}});
        int swings=0,hits=0,bites=0,landed=0,worst=0;
        for(int t=0;t<20*120;++t){world.advance();const auto f=world.state(1);for(const auto& e:f.at("events"))if(e.at("type")=="combat"){
            if(e.at("source")==1){++swings;hits+=!e.value("miss",false);}
            else if(e.at("source")==rat){++bites;if(!e.value("miss",false)){++landed;worst=std::max(worst,e.at("damage").get<int>());}}}}
        const auto s=world.state(1).at("character").at("state");
        check(swings>=90 && hits*100>=swings*42 && hits*100<=swings*72,"an L1 hits a Fare about Saldırı/(Saldırı+defense/3) = 57% of swings");
        check(bites>=80 && landed*100>=bites*30 && landed*100<=bites*65 && worst>=1 && worst<=3,"a Fare bites at attack/3 vs Savunma (48%) for dmgmin-dmgmax/3 (1, crit 2-3)");
        check(s.at("hp").get<int>()>s.at("max_hp").get<int>()/2,"an L1 outlasts two minutes of Fare bites (VOD: never died farming rats)");
    }
    {   // 2 points per level; reaching level 5 refunds Yetenekler and Dirençler with the VOD 49:45 line.
        auto c=config;auto& r=c["npcs"][2];r["x"]=100;r["patrol"]=ikv::Json::array();r["hp"]=1;r["xp"]=2200;
        ikv::World world(c,[](std::span<const ikv::CharacterSave>){});
        auto player=record(1);player["state"]["level"]=4;player["state"]["skills"]={{"depar",2}};player["state"]["resistances"]={{"ates",3}};world.add(1,1,1,player);
        check(world.state(1).at("skill_points")==6,"2 skill points per level: 8 at level 4, 2 spent");
        world.action(1,{{"action","attack"},{"target",rat}});
        std::string line;ikv::Json s;
        for(int t=0;t<100 && line.empty();++t){world.advance();const auto f=world.state(1);s=f;for(const auto& e:f.at("events"))if(e.at("type")=="notice")line=e.at("text");}
        const auto& own=s.at("character").at("state");
        check(own.at("level")==5 && own.at("skills").empty() && own.at("resistances").empty(),"level 5 frees the invested yetenek and direnç points");
        check(line=="5 seviye olduğunuz için yeteneklerinizi/dirençlerinizi ÜCRETSİZ sıfırlandı. Tekrar dağıtabilirsiniz!","VOD 49:45 free reset line");
        check(s.at("skill_points")==10 && s.at("stats").at("resistance_points")==20,"VOD level 5: Kalan 10, Dirençler Puan 20");
    }
    {   // Guide l.1123-1125: sitting refills Enerji and Kudret ten times faster; walking ends the posture.
        auto c=config;c["regeneration"]={{"interval",10},{"hp",3},{"mana",2},{"posture_multiplier",10}};
        ikv::World world(c,[](std::span<const ikv::CharacterSave>){});
        auto player=record(1,-500);player["state"]["hp"]=50;player["state"]["mana"]=0;world.add(1,1,1,player);
        const auto own=[&]{return world.state(1).at("character").at("state");};
        while(own().at("hp")==50)world.advance(); // align with the regeneration interval
        world.action(1,{{"action","chat"},{"channel","local"},{"text","/otur"}});
        const int hp=own().at("hp"),mana=own().at("mana");ticks(world,10);
        check(own().at("hp")==hp+30 && own().at("mana")==mana+20,"a sitting character regenerates x10");
        world.destination(1,-900,0,1);ticks(world,10);
        check(own().at("hp")==hp+33,"standing up returns to the normal rate");
    }
    {   // Death fee 5-3.5L+1.5L² (VOD 8/15/25 at 3/4/5), Plus a third; rise at the nearest Ruh Taşı arrival instead of the spawn.
        auto c=config;auto& r=c["npcs"][2];r["x"]=2900;r["patrol"]=ikv::Json::array();r["damage_range"]={3000.0,3000.0};
        c["npcs"].push_back({{"id",2147493660u},{"source_uid",30002},{"source_class",1},{"kind","door"},{"name","Ruh Taşı"},{"zone","old_town"},{"x",500},{"y",0},{"hp",100},
                             {"dialogue","Ruh Taşı"},{"quests",ikv::Json::array()},{"shop",ikv::Json::array()},{"door",{{"to","eminonu"},{"arrive",{3000,0}}}}});
        ikv::World world(c,[](std::span<const ikv::CharacterSave>){});
        for(int id:{1,2}){auto p=record(id,2800);p["state"]["level"]=id==1?5:3;p["state"]["currency"]=100;world.add(id,id,id,p);}
        world.entitlement(2,true);
        const auto own=[&](int id){return world.state(id).at("character").at("state");};
        for(int t=0;t<200 && (own(1).at("hp")!=0 || own(2).at("hp")!=0);++t)world.advance();
        check(own(1).at("currency")==75 && own(2).at("currency")==98,"level 5 loses 25 Para; a level-3 Plus holder 8/3");
        ticks(world,c.at("death_resurrect_delay").get<int>());world.action(1,{{"action","resurrect"}});
        check(own(1).at("x")==3000 && own(1).at("y")==0,"resurrection at the nearest safe point (a Ruh Taşı arrival)");
    }
}
// Wave 16 see-through doorways. A point on the walkable apron of a door in view (the approach rectangle in front of its
// threshold, "apron"[0] deep).
bool on_apron(const ikv::Json& frame, double x, double y) {
    for (const auto& e : frame.at("entities")) if (e.contains("portal") && e.at("portal").is_object()) {
        const auto& p=e.at("portal"); const auto& t=p.at("threshold"); const double ax=t[0][0],ay=t[0][1],bx=t[1][0],by=t[1][1],w=std::hypot(bx-ax,by-ay);
        const double ux=(bx-ax)/w,uy=(by-ay)/w,dx=x-(ax+bx)/2,dy=y-(ay+by)/2,ahead=ux*dy-uy*dx;
        if (ahead<=0 && ahead>=-p.at("apron")[0].get<double>() && std::abs(ux*dx+uy*dy)<=w/2) return true;
    }
    return false;
}
// Rigid map of an open portal record: threshold midpoint onto exit midpoint, a->b onto exit c->d.
std::pair<double,double> portal_map(const ikv::Json& p, double x, double y) {
    const auto& t=p.at("threshold"); const auto& e=p.at("exit");
    const double ax=t[0][0],ay=t[0][1],bx=t[1][0],by=t[1][1],cx=e[0][0],cy=e[0][1],dx=e[1][0],dy=e[1][1];
    const double th=std::atan2(dy-cy,dx-cx)-std::atan2(by-ay,bx-ax),c=std::cos(th),s=std::sin(th),ux=x-(ax+bx)/2,uy=y-(ay+by)/2;
    return {(cx+dx)/2+c*ux-s*uy,(cy+dy)/2+s*ux+c*uy};
}
// The real config and original walk grids (tools/data/portals.json): a routed walk through each authored pair, both ways, crosses
// once, lands by the door's rigid map on supported cells and reaches its target; Umut Dergisi stays shut (no crossing, no linked
// view, no events through it) until q282 is taken; linked players are tagged, in the snapshot and not party rows.
void portal_walks(const ikv::Json& config, const std::filesystem::path& assets) {
    if (!std::filesystem::exists(assets / "zone00.bin")) { std::cout << "portal_walks skipped: no original walk grids in " << assets << '\n'; return; }
    std::map<std::string,std::shared_ptr<const ikv::Zone>> grids; std::map<unsigned,std::shared_ptr<const ikv::Zone>> support;
    for (const auto& [name,zone] : config.at("zones").items()) if (zone.contains("source_registry_id")) {
        const auto id=ikv::source_scene_descriptor(zone.at("source_registry_id").get<unsigned>()).support_id;
        auto& grid=support[id]; if (!grid) grid=std::make_shared<const ikv::Zone>(ikv::load_zone(assets,id)); grids[name]=grid;
    }
    ikv::World world(config,[](std::span<const ikv::CharacterSave>){}); world.set_source_zones(grids);
    const auto arrive=[&](uint32_t door) { for (const auto& n : config.at("npcs")) if (n.at("id")==door) return n.at("door").at("arrive"); throw std::runtime_error("door missing"); };
    const auto admit=[&](uint32_t id, const char* zone, const ikv::Json& at, ikv::Json quests=ikv::Json::object()) {
        auto r=record(int(id)); r["state"]={{"zone",zone},{"x",at[0]},{"y",at[1]},{"quests",quests}}; world.add(id,id,id,r);
    };
    const auto supported=[&](const std::string& zone, const ikv::Json& s) { return ikv::zone_height(*grids.at(zone),s.at("x").get<float>()/1000.0f,s.at("y").get<float>()/1000.0f).has_value(); };
    uint32_t sequence=1;
    // Walk `id` through `door` (in view) to 5-8 units behind its exit; the frame before the crossing step maps within one step.
    const auto walk=[&](uint32_t id, uint32_t door, const std::string& to) {
        auto frame=world.state(id); const std::string from=frame.at("zone"); const auto p=entity(frame,door).at("portal");
        check(p.at("open")==true && p.at("zone")==to && p.at("floor").size()==2 && p.at("height").get<int>()>=1000,"open see-through door record names its linked zone");
        const auto& e=p.at("exit"); const double cx=e[0][0],cy=e[0][1],dx=e[1][0],dy=e[1][1],w=std::hypot(dx-cx,dy-cy);
        ikv::Json target;
        for (const double depth : {8000.0,5000.0,3000.0}) { target={{"x",std::lround((cx+dx)/2-(dy-cy)/w*depth)},{"y",std::lround((cy+dy)/2+(dx-cx)/w*depth)}}; if (supported(to,target)) break; }
        world.destination(id,target.at("x"),target.at("y"),++sequence,door);
        int crossed=0; ikv::Json last=frame.at("character").at("state");
        for (int tick=0;tick<600;++tick) {
            world.advance(); frame=world.state(id); const auto& s=frame.at("character").at("state");
            check(!event(frame,"correction") && !event(frame,"error"),"a portal walk never corrects");
            if (frame.contains("crossed")) {
                ++crossed; check(frame.at("crossed").at("door")==door && frame.at("zone")==to && last.at("zone")==from,"crossed names the door of the previous zone");
                const auto [mx,my]=portal_map(p,last.at("x"),last.at("y"));
                check(std::hypot(mx-s.at("x").get<double>(),my-s.at("y").get<double>())<=801 && (supported(to,s) || on_apron(frame,s.at("x"),s.at("y"))),"the crossing lands by the door's rigid map on a supported cell or a door apron");
            }
            last=s; if (s.at("zone")==to && s.at("x")==target.at("x") && s.at("y")==target.at("y")) break;
        }
        check(crossed==1 && last.at("zone")==to && last.at("x")==target.at("x") && last.at("y")==target.at("y"),"a routed portal walk crosses once and reaches its target behind the door");
    };
    // Antrepo 30005<->30006 and Mısır Balıkpazarı 30000<->30001, both ways.
    admit(1,"eminonu",arrive(2147493938u)); walk(1,2147493937u,"antrepo"); walk(1,2147493938u,"eminonu");
    admit(2,"eminonu",arrive(2147493934u)); walk(2,2147493933u,"misir"); walk(2,2147493934u,"eminonu");
    // Linked view: player 3 inside the Antrepo is seen from Eminönü through the open door (raw antrepo coordinates, snapshot too).
    admit(3,"antrepo",arrive(2147493937u)); world.state(1);
    const auto seen=world.state(1); bool through_door=false; for (const auto& v : seen.at("entities")) through_door=through_door || (v.at("id")==3 && v.value("portal",0u)==2147493937u && v.at("x")==arrive(2147493937u)[0]);
    const auto snap=world.snapshot(1); check(through_door && std::any_of(snap.begin(),snap.end(),[](const ikv::Player& v){return v.id==3;}),"a player behind an open see-through door is listed with its portal tag");
    world.action(1,{{"action","group_invite"},{"target",3}}); world.action(3,{{"action","group_accept"}});
    check(world.state(1).at("party").empty(),"a group member seen through a door is an entity, not a party row");
    // Umut Dergisi 132061<->132178: quest 282 (q282) gates it.
    const ikv::Json outside=arrive(2147493988u), inside=arrive(2147493987u);
    admit(4,"karakoy",outside); admit(5,"karakoyic",{-774784,-38725}); // 5: in the room, 5 units behind the door
    check(supported("karakoyic",world.state(5).at("character").at("state")),"room spot supported");
    auto shut=world.state(4); const auto closed=entity(shut,2147493987u).at("portal");
    check(closed.at("open")==false && closed.at("reason").get<std::string>().find("görevi")!=std::string::npos && !closed.contains("exit"),"a quest-gated see-through door is shut without the quest");
    check(std::none_of(shut.at("entities").begin(),shut.at("entities").end(),[](const ikv::Json& v){return v.at("id")==5;}),"nothing is seen through a shut door");
    world.destination(4,inside[0],inside[1],++sequence,2147493987u); shut=world.state(4);
    check(event(shut,"error","quest") && event(shut,"correction") && shut.at("zone")=="karakoy","a shut see-through door refuses a walk with the gate's reason");
    admit(6,"karakoy",outside,{{"q282",{{"progress",0}}}}); world.state(6);
    world.action(5,{{"action","chat"},{"channel","local"},{"text","/otur"}});
    check(event(world.state(6),"emote") && !event(world.state(4),"emote"),"events reach observers through an open door only");
    check(entity(world.state(6),5).value("portal",0u)==2147493987u,"the taken quest opens Umut Dergisi");
    walk(6,2147493987u,"karakoyic"); walk(6,2147493988u,"karakoy");
}
// Real content: a crafter at economy.recipe_limit (the largest recipes) with a full bag of the largest items, at the
// Eminönü banker with a full Banka tab of the largest items open. The recipes/bank travel as Type::catalog parts, each
// within one frame, so the state frame fits and keeps the banker (an NPC) in view; before they rode the state and threw.
void crafter_frame(const ikv::Json& config, const std::filesystem::path& assets) {
    if (!std::filesystem::exists(assets / "zone00.bin")) { std::cout << "crafter_frame skipped: no original walk grids in " << assets << '\n'; return; }
    std::map<std::string,std::shared_ptr<const ikv::Zone>> grids; std::map<unsigned,std::shared_ptr<const ikv::Zone>> support;
    for (const auto& [name,zone] : config.at("zones").items()) if (zone.contains("source_registry_id")) {
        const auto id=ikv::source_scene_descriptor(zone.at("source_registry_id").get<unsigned>()).support_id;
        auto& grid=support[id]; if (!grid) grid=std::make_shared<const ikv::Zone>(ikv::load_zone(assets,id)); grids[name]=grid;
    }
    ikv::World world(config,[](std::span<const ikv::CharacterSave>){}); world.set_source_zones(grids);
    const auto& items=config.at("items"); const auto size=[](const ikv::Json& v){return v.dump().size();};
    std::vector<std::pair<size_t,std::string>> recipes,largest;
    for (const auto& [id,r] : config.at("recipes").items()) {
        size_t bytes=size({{id,r}})+size({{r.at("product").get<std::string>(),items.at(r.at("product").get<std::string>())}});
        for (const auto& c : r.at("components")) bytes+=size({{c.at("item").get<std::string>(),items.at(c.at("item").get<std::string>())}});
        recipes.push_back({bytes,id});
    }
    for (const auto& [id,item] : items.items()) largest.push_back({size({{id,item}}),id});
    std::sort(recipes.rbegin(),recipes.rend()); std::sort(largest.rbegin(),largest.rend());
    const auto limit=config.at("economy").at("recipe_limit").get<size_t>(), slots=config.at("economy").at("bank_slots").get<size_t>();
    check(recipes.size()>=limit && largest.size()>=32+slots,"real content has enough recipes and items");
    const auto hex=[](char lead,size_t n){std::string u(30,lead);const char* d="0123456789abcdef";return u+d[n/16]+d[n%16];};
    ikv::Json known=ikv::Json::array(),bag=ikv::Json::array(),stored=ikv::Json::array();
    for (size_t n=0;n<limit;++n) known.push_back(recipes[n].second);
    for (size_t n=0;n<32;++n) bag.push_back({{"uid",hex('a',n)},{"item",largest[n].second},{"quantity",1}});
    for (size_t n=0;n<slots;++n) stored.push_back({{"uid",hex('b',n)},{"item",largest[32+n].second},{"quantity",1}});
    uint32_t banker=0; ikv::Json at;
    for (const auto& n : config.at("npcs")) if (n.value("banker",false) && n.at("zone")=="eminonu") { banker=n.at("id"); at={n.at("x"),n.at("y").get<int>()+100}; break; }
    check(banker,"real content has an Eminönü banker");
    auto r=record(1); r["state"]={{"zone","eminonu"},{"x",at[0]},{"y",at[1]},{"recipes",known},{"inventory",bag}}; r["bank"]={{"items",stored},{"currency",0}};
    world.add(1,1,1,r); world.action(1,{{"action","bank_open"},{"target",banker}});
    const auto frame=world.state(1); const auto book=world.recipe_book(1), bank=world.bank_view(1);
    check(frame.at("character").at("state").at("recipes").size()==limit && frame.at("character").at("state").at("inventory").size()==32 &&
          !bank.at("bank").is_null() && bank.at("bank").at("items").size()==slots && book.at("recipes").size()==limit,"crafter at every configured limit");
    check(size(frame)<=65533 && size(book)<=65533 && size(bank)<=65533,"state and each catalog part fit one frame");
    check(std::any_of(frame.at("entities").begin(),frame.at("entities").end(),[&](const ikv::Json& e){return e.at("id")==banker;}),"the banker stays in view beside a full recipe memory and bank");
}
// Instance copies behind a see-through door (synthetic grid, NOT ORIGINAL): each group sees and enters its own copy, another
// group's peeked copy counts as occupied, the copy's NPCs show through the door and the copy's door (pair + n*65536) leads back;
// a copy left through the door keeps its state while peeked and resets once the last peek drops (vacated()); a crowded frame
// never carries a linked row without its door (the 96-entity cut and the byte budget).
void portal_copies(const ikv::Json& mechanics) {
    auto grid=std::make_shared<ikv::Zone>(); grid->width=grid->height=2048; grid->source="SYNTHETIC NOT ORIGINAL"; grid->cell_size=1.5f;
    grid->cells.assign(2048u*2048u,0); grid->heights.assign(grid->cells.size(),0.0f);
    for (int y=-6;y<=6;++y) for (int x=0;x<=10;++x) grid->cells[size_t(y+1024)*2048+size_t(x+1024)]=255; // a room; doors in its east/west walls
    for (int x=-100;x<0;++x) grid->cells[1024u*2048u+size_t(x+1024)]=255; // a corridor west, out of aoi of the Eminönü door
    auto config=mechanics;
    for (const auto& [key,value] : std::initializer_list<std::pair<const char*,int>>{{"movement_step",800},{"cell_size",20000},{"aoi",150000},{"interaction_range",15000}}) config[key]=value;
    const ikv::Json bounds={-1536000,-1536000,1534500,1534500};
    config["zones"]={{"eminonu",{{"source_registry_id",0},{"bounds",bounds},{"spawn",{0,0}},{"collision",ikv::Json::array()}}},
                     {"migrat",{{"source_registry_id",36},{"instances",{27,28}},{"bounds",bounds},{"spawn",{3000,0}},{"collision",ikv::Json::array()}}},
                     {"old_town",mechanics.at("zones").at("old_town")}};
    const auto door=[](uint32_t id, const char* zone, int x, const char* to, int arrive, uint32_t pair, ikv::Json threshold) {
        return ikv::Json{{"id",id},{"kind","door"},{"name","Kapı"},{"zone",zone},{"x",x},{"y",0},{"hp",100},{"dialogue","Kapı"},{"quests",ikv::Json::array()},{"shop",ikv::Json::array()},
                         {"door",{{"to",to},{"arrive",{arrive,0}},{"portal",{{"pair",pair},{"threshold",threshold},{"floor",0},{"height",12000},{"apron",0}}}}}};
    };
    auto rat=mechanics.at("npcs")[2]; rat["zone"]="migrat"; rat["x"]=4500; rat["y"]=3000; rat["patrol"]=ikv::Json::array(); rat["hp"]=1; rat["respawn"]=72000; // dies at a hit, no respawn
    config["npcs"]={door(2147493700u,"eminonu",15000,"migrat",1500,2147493701u,{{15750,2250},{15750,-2250}}),door(2147493701u,"migrat",0,"eminonu",13500,2147493700u,{{-750,-2250},{-750,2250}}),rat};
    config["quests"]=ikv::Json::object(); config["defaults"]["zone"]="eminonu"; config["defaults"]["x"]=0; config["defaults"]["y"]=0;
    ikv::World world(config,[](std::span<const ikv::CharacterSave>){}); world.set_source_zones({{"eminonu",grid},{"migrat",grid}});
    for (uint32_t p=1;p<=5;++p) world.add(p,p,p,record(int(p),12000));
    for (const auto& [leader,member] : {std::pair{1u,2u},{4u,5u}}) { world.action(leader,{{"action","group_invite"},{"target",member}}); world.action(member,{{"action","group_accept"}}); }
    const auto shown=[&](uint32_t p){ return entity(world.state(p),2147493700u).at("portal"); };
    check(shown(1).at("zone")=="migrat1" && shown(2).at("zone")=="migrat1" && shown(4).at("zone")=="migrat2" && shown(3).at("zone")=="migrat","each group sees its own copy through the door (another group's peek counts as occupied), solo players the shared zone");
    check(shown(1).at("registry")==27 && entity(world.state(1),2147493651u+65536u).value("portal",0u)==2147493700u,"the copy's own NPCs show through the door");
    world.destination(1,6000,0,1,2147493700u); bool crossed=false;
    for (int tick=0;tick<40 && !crossed;++tick) { world.advance(); crossed=world.state(1).contains("crossed"); }
    auto inside=world.state(1); check(crossed && inside.at("zone")=="migrat1","the walk enters the copy the door showed");
    const auto back=entity(inside,2147493701u+65536u).at("portal");
    check(back.at("open")==true && back.at("zone")=="eminonu" && entity(inside,2).value("portal",0u)==2147493701u+65536u,"the copy's door (pair + n*65536) shows the way back");
    const uint32_t rat1=2147493651u+65536u; uint32_t sequence=1;
    const auto until=[&](uint32_t p,auto done,int limit){ for (int tick=0;tick<limit;++tick) { world.advance(); world.state(2); if (done(world.state(p))) return; } throw std::runtime_error("portal copy step timed out"); };
    world.action(1,{{"action","attack"},{"target",rat1}});
    until(1,[&](const ikv::Json& f){ return !entity(f,rat1).at("alive").get<bool>(); },600);
    world.destination(1,12000,0,++sequence,2147493701u+65536u);
    until(1,[&](const ikv::Json& f){ return f.at("zone")=="eminonu" && f.at("character").at("state").at("x")==12000; },200);
    check(!entity(world.state(1),rat1).at("alive").get<bool>(),"a copy left through the door keeps its state while it is peeked");
    for (uint32_t p : {1u,2u}) world.destination(p,-138000,0,++sequence); // both peekers leave the door's aoi
    until(1,[&](const ikv::Json& f){ return f.at("character").at("state").at("x")==-138000 && world.state(2).at("character").at("state").at("x")==-138000; },400);
    world.destination(1,12000,0,++sequence);
    until(1,[&](const ikv::Json& f){ return f.at("character").at("state").at("x")==12000; },400);
    const auto again=world.state(1);
    check(entity(again,2147493700u).at("portal").at("zone")=="migrat1" && entity(again,rat1).at("alive").get<bool>(),"a copy resets (vacated) once it drops out of every peek");
    // Crowded frames: every row tagged with a door must have that door in the same frame (client validate_state).
    const auto tags_ok=[](const ikv::Json& f){
        const auto& list=f.at("entities");
        return std::all_of(list.begin(),list.end(),[&](const ikv::Json& e){ return !e.contains("portal") || !e.at("portal").is_number() ||
            std::any_of(list.begin(),list.end(),[&](const ikv::Json& d){ return d.at("id")==e.at("portal") && d.at("portal").is_object() && d.at("portal").at("open")==true; }); }); };
    const auto crowd_frame=[&](const ikv::Json& crowd) {
        ikv::World w(crowd,[](std::span<const ikv::CharacterSave>){}); w.set_source_zones({{"eminonu",grid},{"migrat",grid}});
        w.add(1,1,1,record(1,12000)); auto inside_record=record(2,3000); inside_record["state"]["zone"]="migrat"; w.add(2,2,2,inside_record);
        auto f=w.state(1); check(tags_ok(f),"no linked row without its door in a crowded frame"); return f;
    };
    auto crowd=config; // 100 props nearer than the door: the 96-entity cut must not take the door
    for (uint32_t k=0;k<100;++k) crowd["npcs"].push_back({{"id",2147494000u+k},{"kind","prop"},{"name","Kasa"},{"zone","eminonu"},{"x",12000},{"y",1500},{"hp",1},{"dialogue","Kasa"},{"quests",ikv::Json::array()},{"shop",ikv::Json::array()}});
    auto frame=crowd_frame(crowd);
    check(frame.at("entities").size()==96 && entity(frame,2147493700u).at("portal").at("open")==true && entity(frame,2).value("portal",0u)==2147493700u,"a see-through door survives the 96-entity cut ahead of nearer NPCs");
    auto heavy=config; heavy["inventory_slots"]=2; // a door whose shop catalog alone exceeds the frame: its linked rows go too
    for (int k=0;k<200;++k) { const auto id="big"+std::to_string(k); heavy["items"][id]={{"name","Büyük Sandık"},{"stack",1},{"price",1},{"level",1},{"detail",std::string(256,'x')}}; heavy["npcs"][0]["shop"].push_back(id); }
    frame=crowd_frame(heavy);
    check(std::none_of(frame.at("entities").begin(),frame.at("entities").end(),[](const ikv::Json& e){ return e.at("id")==2147493700u || e.at("id")==2; }),"rows linked through a door the byte budget cut are dropped with it");
}
int main(int argc, char** argv) {
    try {
        if (argc != 3) throw std::runtime_error("usage: world-test config/gameplay.json tests/independent_content.json");
        std::ifstream input(argv[1]); ikv::Json config; input >> config;
        portal_walks(config,std::filesystem::path(argv[1]).parent_path().parent_path()/"original"/"app"/"base");
        crafter_frame(config,std::filesystem::path(argv[1]).parent_path().parent_path()/"original"/"app"/"base");
        std::ifstream fixture_input(argv[2]); ikv::Json fixture; fixture_input >> fixture;
        config=independent_mechanics(std::move(config),fixture);
        // Arbitrary synthetic visual item metadata, not a canonical weapon-slot mapping.
        config["items"]["training_blade"]["source_uid"]=4002;
        config["items"]["training_blade"]["source_slot"]=1;
        std::vector<ikv::CharacterSave> saved; bool fail = false; int calls = 0;
        ikv::World world(config, [&](std::span<const ikv::CharacterSave> states) {
            ++calls; if (fail) throw std::runtime_error("injected persistence failure");
            saved.assign(states.begin(), states.end());
        });
        world.add(1, 1, 1, record(1)); world.add(2, 2, 2, record(2));
        visible_cosmetics(config);
        crowd_and_gear(config);
        social_rules(config);
        clan_rules(config);
        door_rules(config);
        activity_rules(config);
        boss_rules(config,false);boss_rules(config,true);
        idle_walk_rules(config);
        regeneration_rules(config);
        progression_rules(config);
        actionbar_rules(config);
        glyph_riders(config);
        item_rules(config);
        point_reset_rules(config);
        quest_rules(config);
        quest_objectives(config);
        starter_kit(config);
        bot_rules(config);bot_fairness(config);bot_claim(config);bot_spawn_spread(config);
        portal_copies(config);
        check(world.state(1).at("character").at("state").at("currency") == 100, "default currency");
        check(world.state(1).at("clock").at("interior")==ikv::Json::array({200,157,142,126,105,110,184,199,217,78,0}),"roofed zones get the interior_hour (noon) record at every hour");
        world.action(1, {{"action", "buy"}, {"target", 2147493650u}, {"item", "healing_tonic"}, {"quantity", 1}});
        check(event(world.state(1), "error", "range"), "merchant range");
        world.destination(1, 200, 0, 1); ticks(world, 2);
        world.action(1, {{"action", "quest_accept"}, {"target", 2147493649u}, {"quest", "first_hunt"}});
        check(event(world.state(1), "ack"), "accept quest");
        {
            const auto quest=world.state(1).at("catalogs").at("quests").at("first_hunt");
            check(quest.at("objective_text").is_string() && quest.at("description").is_string(),"quest window texts in state catalog");
            world.action(1,{{"action","interact"},{"target",2147493649u}});
            bool offered=false; const auto talk=world.state(1);
            for(const auto& e:talk.at("events"))if(e.at("type")=="dialogue")offered=e.at("quests")==ikv::Json::array({"first_hunt"}) && e.at("text").is_string();
            check(offered,"questgiver dialogue offers its quests");
            world.action(1,{{"action","quest_abandon"},{"quest","first_hunt"}});
            check(!world.state(1).at("character").at("state").at("quests").contains("first_hunt"),"Seçili görevi sil drops an unfinished quest");
            world.action(1,{{"action","quest_abandon"},{"quest","first_hunt"}});
            check(event(world.state(1),"error","quest"),"abandon requires an unfinished quest");
            world.action(1, {{"action", "quest_accept"}, {"target", 2147493649u}, {"quest", "first_hunt"}});
            check(event(world.state(1), "ack"), "abandoned quest can be accepted again");
        }
        // Click-to-attack from outside weapon reach: the server walks the player in and repeats swings until the kill.
        world.action(1, {{"action", "attack"}, {"target", 2147493651u}}); ticks(world, 40);
        auto killed=world.state(1);
        check(killed.at("character").at("state").at("x").get<int>() > 200, "auto-attack approaches the target");
        check(killed.at("character").at("state").at("quests").at("first_hunt").at("progress") == 1, "kill objective");
        check(authority_event(killed,"combat",1,1,2147493651u) && authority_event(killed,"death",2147493651u,1,2147493651u),
              "combat and NPC death project high uint32 target identity");
        check(entity(killed,2147493651u).at("alive")==false,"NPC death changes visible life state");
        world.action(2, {{"action", "loot"}, {"target", 2147493651u}});
        check(event(world.state(2), "error"), "loot ownership or range");
        world.action(1, {{"action", "loot"}, {"target", 2147493651u}}); ticks(world, 5); // walks to the Ganimet and opens its window
        check(event(world.state(1), "loot_open") && !holds(world.state(1), "rat_tail"), "opening the Ganimet lists its slots");
        world.action(1, {{"action", "loot_take"}, {"target", 2147493651u}, {"slot", 99}});
        check(event(world.state(1), "error", "slot"), "empty Ganimet slot rejected");
        for (unsigned slot = 0; slot < 9; ++slot) world.action(1, {{"action", "loot_take"}, {"target", 2147493651u}, {"slot", slot}});
        check(holds(world.state(1), "rat_tail") && entity(world.state(1),2147493651u).at("looted")==true, "slot taken; emptied Ganimet is looted");
        world.action(1, {{"action", "loot"}, {"target", 2147493651u}});
        check(event(world.state(1), "error"), "no duplicate loot");
        world.destination(1, 200, 0, 3); ticks(world, 6);
        world.action(1, {{"action", "quest_complete"}, {"target", 2147493649u}, {"quest", "first_hunt"}});
        auto reward = world.state(1);
        check(reward.at("character").at("state").at("quests_done")==ikv::Json::array({"first_hunt"}) && !reward.at("character").at("state").at("quests").contains("first_hunt"), "quest reward");
        check(reward.at("character").at("state").at("level")==2 && reward.at("character").at("state").at("xp")==30, "quest XP progression (rat 30 + reward 1500 over the 1500 level-1 interval)");
        world.action(1, {{"action", "quest_complete"}, {"target", 2147493649u}, {"quest", "first_hunt"}});
        check(event(world.state(1), "error"), "reward cannot repeat");
        {
            const int level=world.state(1).at("character").at("state").at("level");
            auto stats=world.state(1).at("stats");
            check(stats.at("resistance_points")==5*(level-1) && stats.at("craft_points")==2*(level-1) && stats.at("weapon_damage").size()==2,"per-level resistance/craft points and derived stats");
            world.action(1,{{"action","stat_points"},{"points",{{"ates",1}}}});
            auto spent=world.state(1);
            check(spent.at("character").at("state").at("resistances").at("ates")==1 && spent.at("stats").at("resistance_points")==5*(level-1)-1,"spend a resistance point");
            world.action(1,{{"action","stat_points"},{"points",{{"ates",-1}}}});
            check(event(world.state(1),"error","quantity") && world.state(1).at("character").at("state").at("resistances").at("ates")==1,"invested points are not refunded (original ✓ commits)");
            world.action(1,{{"action","stat_points"},{"points",{{"guc",1}}}});
            check(event(world.state(1),"error","stat"),"unknown point rejected");
            world.action(1,{{"action","stat_points"},{"points",{{"buz",5*(level-1)}}}});
            check(event(world.state(1),"error","points") && !world.state(1).at("character").at("state").at("resistances").contains("buz"),"a commit within the unspent pool only");
            world.action(1,{{"action","stat_points"},{"points",{{"madenci",2*(level-1)}}}});
            world.action(1,{{"action","stat_points"},{"points",{{"lokman",1}}}});
            auto full=world.state(1);
            check(event(full,"error","points") && full.at("stats").at("craft_points")==0 && full.at("character").at("state").at("crafts").at("madenci")==2*(level-1),
                  "Toplama shares the bounded Üretim pool");
        }
        world.destination(1, 400, 0, 4); ticks(world, 2);
        auto before = world.state(1).at("character").at("state"); fail = true;
        world.action(1, {{"action", "buy"}, {"target", 2147493650u}, {"item", "healing_tonic"}, {"quantity", 1}});
        check(event(world.state(1), "error", "persistence"), "purchase persistence rejection");
        check(world.state(1).at("character").at("state") == before, "purchase rollback"); fail = false;
        world.action(1, {{"action", "buy"}, {"target", 2147493650u}, {"item", "healing_tonic"}, {"quantity", 1}});
        auto tonic = uid(world.state(1), "healing_tonic");
        world.action(1, {{"action", "sell"}, {"target", 2147493650u}, {"uid", tonic}, {"quantity", 1}});
        check(world.state(1).at("character").at("state").at("currency") == before.at("currency").get<int>() - 8, "vendor fifth price");
        world.destination(2, 400, 0, 1); ticks(world, 4);
        world.action(1, {{"action", "trade_request"}, {"target", 2}});
        auto invitation=terms(world.state(2));
        check(invitation.at("phase")=="invited" && invitation.at("initiator")==1 && invitation.at("revision")==1,"invitation role and revision");
        check(invitation.at("own").at("items").empty() && invitation.at("other").at("items").empty() &&
              invitation.at("own").at("currency")==0 && invitation.at("other").at("currency")==0,"empty authoritative offers");
        world.action(2, {{"action", "trade_accept"}});
        check(event(world.state(2),"error","trade_revision"),"missing invitation revision rejected");
        world.action(2, {{"action", "trade_accept"},{"revision",1}});
        auto opened=terms(world.state(1));
        check(opened.at("phase")=="open" && opened.at("revision")==1,"invitation consent opens exchange");
        world.action(1, {{"action", "trade_offer"}, {"items", ikv::Json::array({{{"uid", tonic}, {"quantity", 1}}})}, {"currency", 7}});
        auto peer_terms=terms(world.state(2));auto own_terms=terms(world.state(1));
        check(peer_terms.at("revision")==2 && peer_terms.at("other").at("currency")==7 && peer_terms.at("own").at("currency")==0,"peer authoritative currency");
        const auto& offered=peer_terms.at("other").at("items").at(0);
        check(offered.at("uid")==tonic && offered.at("item")=="healing_tonic" && offered.at("quantity")==1 &&
              offered.at("name")==config.at("items").at("healing_tonic").at("name"),"trusted item identity and display name");
        check(own_terms.at("own")==peer_terms.at("other") && own_terms.at("other")==peer_terms.at("own"),"terms are symmetric perspectives");
        world.action(1, {{"action", "trade_accept"},{"revision",2}});
        auto accepted=terms(world.state(1));
        check(accepted.at("own").at("accepted")==true && accepted.at("other").at("accepted")==false,"both confirmation states visible");
        world.action(2, {{"action", "trade_offer"}, {"items", ikv::Json::array()}, {"currency", 3}});
        auto changed=terms(world.state(1));
        check(changed.at("revision")==3 && changed.at("own").at("accepted")==false && changed.at("other").at("accepted")==false,"offer edit clears both confirmations");
        const int old_calls = calls;
        world.action(1, {{"action", "trade_accept"},{"revision",2}});
        check(event(world.state(1),"error","trade_revision") && calls==old_calls,"stale displayed terms cannot authorize trade");
        world.action(2, {{"action", "trade_accept"},{"revision",3}});
        check(calls == old_calls, "offer change invalidates both accepts");
        auto one = world.state(1).at("character").at("state"), two = world.state(2).at("character").at("state");
        fail = true;
        world.action(1, {{"action", "trade_accept"},{"revision",3}});
        auto failure=world.state(1);
        check(event(failure, "error", "trade_persistence"), "trade persistence rejection");
        auto failed_self=terms(failure);
        check(failed_self.at("own").at("accepted")==false && failed_self.at("other").at("accepted")==true &&
              failed_self.at("revision")==3 && failed_self.at("phase")=="open","DB failure refreshes unchanged authoritative consent");
        for(const auto& value:failure.at("events"))if(value.at("type")=="error" && value.at("code")=="trade_persistence")
            check(value.at("peer")==2 && value.at("revision")==3,"commit failure attributable to displayed exchange");
        check(world.state(1).at("character").at("state") == one && world.state(2).at("character").at("state") == two, "two-party rollback");
        world.action(2, {{"action", "trade_accept"},{"revision",3}});
        auto failed_terms=terms(world.state(2));
        check(failed_terms.at("own").at("accepted")==true && failed_terms.at("other").at("accepted")==false &&
              failed_terms.at("phase")=="open" && failed_terms.at("revision")==3,"failed DB commit preserves prior consent and revision");
        fail = false; world.action(1, {{"action", "trade_accept"},{"revision",3}});
        check(saved.size() == 2, "single atomic callback for both players");
        check(world.state(1).at("character").at("state").at("currency") == one.at("currency").get<int>() - 4, "trade currency conserved");
        world.action(1, {{"action", "chat"}, {"channel", "private"}, {"to", 2}, {"text", "merhaba"}});
        check(event(world.state(2), "chat"), "private chat");
        world.action(1, {{"action", "group_invite"}, {"target", 2}}); world.action(2, {{"action", "group_accept"}});
        ticks(world,10);
        world.action(1, {{"action", "chat"}, {"channel", "group"}, {"text", "grup"}});
        check(event(world.state(2), "chat"), "group chat");
        ticks(world,10); world.action(1, {{"action", "chat"}, {"channel", "zone"}, {"text", "bölge"}});
        check(event(world.state(1), "error", "plus") && !event(world.state(2), "chat"), "Bölge needs Plus Paket (guide)");
        world.entitlement(1, true); world.action(1, {{"action", "chat"}, {"channel", "zone"}, {"text", "bölge"}});
        check(event(world.state(2), "chat"), "zone chat");
        world.entitlement(1, false);
        world.action(1, {{"action", "moderate"}, {"target", 2}, {"operation", "kick"}});
        check(event(world.state(1), "error", "permission"), "moderation RBAC");
        world.destination(1, 100000, 0, 5); check(event(world.state(1), "correction"), "bounds correction");
        world.action(1, {{"action", "buy"}, {"target", 2147493650u}, {"item", "healing_tonic"}, {"quantity", -1}});
        check(event(world.state(1), "error"), "negative quantity");
        world.save_all(); check(saved.size() == 2, "save all");
        world.remove(1); world.add(3, 1, 1, {{"name", "Player1"}, {"class_id", 1}, {"state", saved.front().state}});
        check(world.state(3).at("character").at("state").at("quests_done")==ikv::Json::array({"first_hunt"}), "persistent reconnect");
        check(world.state(3).at("character").at("state").at("crafts").value("madenci",0)>0,"persistent craft points");
        {
            auto overspent=record(4); overspent["state"]["resistances"]={{"ates",3},{"bogus",1}};
            world.add(4,4,4,overspent);
            check(world.state(4).at("character").at("state").at("resistances").empty(),"overspent or unknown points are refunded at admission");
            world.remove(4);
        }
        auto split_uid=uid(world.state(3),"healing_tonic");
        world.action(3,{{"action","split"},{"uid",split_uid},{"quantity",1}});
        check(event(world.state(3),"ack"),"inventory split");
        world.action(3,{{"action","guild_create"},{"name","Talım"}});
        check(world.state(3).at("character").at("state").at("guild").at("revision")==1,"guild creation revision");
        world.action(3,{{"action","guild_invite"},{"target",2}});world.action(2,{{"action","guild_accept"}});
        check(world.state(2).at("character").at("state").at("guild").at("members").size()==2,"guild join");
        world.action(3,{{"action","chat"},{"channel","guild"},{"text","lonca"}});
        check(event(world.state(3),"error","plus") && !event(world.state(2),"chat"),"Lonca chat needs Plus Paket (guide)");
        world.action(3,{{"action","guild_leave"}});
        check(world.state(2).at("character").at("state").at("guild").at("leader_character_id")==2,"guild succession");
        check(!world.state(3).at("character").at("state").contains("guild"),"guild departure");
        ikv::World mechanics(config,[](std::span<const ikv::CharacterSave>){});
        auto healer=record(10);healer["class_id"]=3;healer["state"]["hp"]=50;healer["state"]["level"]=10;
        mechanics.add(10,10,10,healer,true);mechanics.add(11,11,11,record(11,3000));
        check(mechanics.snapshot(10).size()==1,"spatial AOI excludes far player");
        mechanics.action(10,{{"action","skill"},{"target",10},{"skill","iyilestirme"}});
        check(event(mechanics.state(10),"error","rank"),"skill requires invested points");
        mechanics.action(10,{{"action","skill_learn"},{"skills",{{"meteorit",1}}}});
        check(event(mechanics.state(10),"error","class"),"skill learning restricted to class");
        mechanics.action(10,{{"action","skill_learn"},{"skills",{{"iyilestirme",1},{"ruh_kalkani",1},{"zehirleme",1}}}});
        check(mechanics.state(10).at("character").at("state").at("skills").at("ruh_kalkani")==1,"skill point invested");
        mechanics.action(10,{{"action","skill"},{"target",10},{"skill","sifa_bilgisi"}});
        check(event(mechanics.state(10),"error"),"passive skill cannot be cast");
        mechanics.action(10,{{"action","skill"},{"target",10},{"skill","iyilestirme"}});
        auto healed=mechanics.state(10);
        check(healed.at("character").at("state").at("hp")==50+config.at("skills").at("iyilestirme").at("heal").get<int>(),"healing skill");
        check(authority_event(healed,"skill",10,10,10),"skill projects authoritative source and target");
        check(!event(mechanics.state(11),"skill"),"skill event restricted to spatial AOI");
        mechanics.action(10,{{"action","skill"},{"target",10},{"skill","iyilestirme"}});
        check(event(mechanics.state(10),"error","cooldown"),"skill cooldown");
        mechanics.action(10,{{"action","skill"},{"target",10},{"skill","ruh_kalkani"}});
        check(event(mechanics.state(10),"error","cooldown"),"one-second global skill cooldown");
        ticks(mechanics,20);
        mechanics.action(10,{{"action","skill"},{"target",10},{"skill","ruh_kalkani"}});
        check(mechanics.state(10).at("character").at("state").at("buffs").size()==1,"buff applied");
        ticks(mechanics,600);
        check(mechanics.state(10).at("character").at("state").at("buffs").empty(),"buff expired");
        {
            auto tough=config;tough["npcs"][2]["hp"]=100000; // survives the boosted blow so its damage is observable
            ikv::World skills(tough,[](std::span<const ikv::CharacterSave>){});
            auto warrior=record(20,700);warrior["state"]["level"]=3;skills.add(20,20,20,warrior);
            skills.action(20,{{"action","skill_learn"},{"skills",{{"ofansif_dovus",1},{"sert_vurus",1},{"depar",1}}}});
            skills.action(20,{{"action","skill_learn"},{"skills",{{"depar",4}}}}); // level 3: 2×3 = 6 points, 3 spent
            check(event(skills.state(20),"error","points"),"skill points limited by level");
            skills.action(20,{{"action","skill"},{"target",2147493651u},{"skill","sert_vurus"}});
            check(event(skills.state(20),"error","requires"),"Sert Vuruş requires Ofansif Dövüş");
            skills.action(20,{{"action","skill"},{"target",20},{"skill","ofansif_dovus"}});
            check(skills.state(20).at("character").at("state").at("buffs").at(0).at("stance")=="dovus","stance buff");
            ticks(skills,20);
            skills.action(20,{{"action","skill"},{"target",2147493651u},{"skill","sert_vurus"}});
            auto struck=skills.state(20);
            const int boosted=config.at("skills").at("sert_vurus").at("damage").get<int>()+config.at("skills").at("ofansif_dovus").at("attack").get<int>();
            check(authority_event(struck,"skill",20,20,2147493651u) && entity(struck,2147493651u).at("hp")==100000-boosted,"stance-boosted Sert Vuruş damage");
        }
        {
            // A spell kill (Asit Saldırısı: damage plus acid damage over time) leaves a Ganimet the caster can open.
            auto weak=config;weak["npcs"][2]["hp"]=1;
            ikv::World spells(weak,[](std::span<const ikv::CharacterSave>){});
            auto caster=record(30,700);caster["class_id"]=3;caster["state"]["level"]=20;spells.add(30,30,30,caster);
            spells.action(30,{{"action","skill_learn"},{"skills",{{"asit_saldirisi",1}}}});
            spells.action(30,{{"action","skill"},{"target",2147493651u},{"skill","asit_saldirisi"}});
            check(entity(spells.state(30),2147493651u).at("alive")==false,"spell kill");
            ticks(spells,80);
            spells.action(30,{{"action","loot"},{"target",2147493651u}});ticks(spells,20);
            check(event(spells.state(30),"loot_open"),"spell-killed Ganimet opens for its killer");
        }
        const int before_tonic=mechanics.state(10).at("character").at("state").at("hp");
        mechanics.action(10,{{"action","use"},{"uid",uid(mechanics.state(10),"healing_tonic")}});
        check(mechanics.state(10).at("character").at("state").at("hp")==before_tonic+config.at("items").at("healing_tonic").at("heal").get<int>(),"consumable health");
        mechanics.action(10,{{"action","moderate"},{"target",11},{"operation","mute"}});
        mechanics.action(11,{{"action","chat"},{"channel","local"},{"text","blocked"}});
        check(event(mechanics.state(11),"error","muted"),"admin mute");
        mechanics.action(10,{{"action","moderate"},{"target",11},{"operation","unmute"}});
        mechanics.action(11,{{"action","chat"},{"channel","local"},{"text","allowed"}});
        check(event(mechanics.state(11),"chat"),"admin unmute");
        mechanics.moderation(11,true);
        ticks(mechanics,10);mechanics.action(11,{{"action","chat"},{"channel","local"},{"text","blocked"}});
        check(event(mechanics.state(11),"error","muted"),"external mute independent");
        mechanics.action(10,{{"action","zone"},{"zone","old_town"}});
        check(mechanics.state(10).at("character").at("state").at("zone")=="old_town","zone portal");
        auto dying=record(12,800);dying["state"]["hp"]=1;mechanics.add(12,12,12,dying);
        mechanics.advance();
        auto death=mechanics.state(12);
        check(death.at("character").at("state").at("hp")==0,"NPC death");
        check(authority_event(death,"death",12,2147493651u,12),"player death preserves NPC source authority");
        mechanics.action(12,{{"action","attack"},{"target",2147493651u}});
        check(event(mechanics.state(12),"error","dead"),"dead cannot attack");
        mechanics.action(12,{{"action","resurrect"}});
        check(event(mechanics.state(12),"error","cooldown"),"Canlanma Süresi countdown blocks immediate resurrection");
        mechanics.remove(12);mechanics.add(12,12,12,death.at("character"));mechanics.action(12,{{"action","resurrect"}});
        check(event(mechanics.state(12),"error","cooldown"),"a relog while dead does not skip the Canlanma Süresi countdown");
        ticks(mechanics,20);
        mechanics.action(12,{{"action","resurrect"}});
        auto resurrection=mechanics.state(12);auto revived=resurrection.at("character").at("state");
        check(authority_event(resurrection,"resurrect",12,12,12),"resurrection projects authoritative life transition");
        check(revived.at("hp")==revived.at("max_hp") && revived.at("x")==0 && revived.at("currency")==97,"safe resurrection and the level-1 death fee (3 Para)");
        mechanics.destination(12,1000,500,1);
        check(event(mechanics.state(12),"correction"),"configured collision destination");
        mechanics.action(10,{{"action","moderate"},{"target",11},{"operation","kick"}});
        check(event(mechanics.state(11),"kick"),"admin transport kick event");
        ikv::World grouped(config,[](std::span<const ikv::CharacterSave>){});
        for(int id=1;id<=7;++id)grouped.add(id,id,id,record(id));
        for(int id=2;id<=6;++id){grouped.action(1,{{"action","group_invite"},{"target",id}});grouped.action(id,{{"action","group_accept"}});}
        grouped.action(1,{{"action","group_invite"},{"target",7}});
        check(event(grouped.state(1),"error","group"),"six-member group cap");
        grouped.action(1,{{"action","group_leave"}});
        check(event(grouped.state(2),"group"),"group leader succession");
        auto walls=config;walls["zones"]["eminonu"]["collision"].push_back({50,-10,51,10});
        ikv::World collision(walls,[](std::span<const ikv::CharacterSave>){});
        collision.add(1,1,1,record(1));collision.input(1,1,0,1);collision.advance();
        check(collision.state(1).at("character").at("state").at("x")==0,"thin wall cannot tunnel");
        synthetic_support_navigation(config);
        click_routes(config);
        economy_mechanics(config);
        auto malformed=config;malformed["items"]["healing_tonic"]["stack"]=0;
        bool invalid=false;try{ikv::World rejected(malformed,[](std::span<const ikv::CharacterSave>){});}catch(const std::invalid_argument&){invalid=true;}
        check(invalid,"malformed configuration rejected");
        auto slots=config;
        for(int n=1;n<=8;++n) {
            auto item="slot_item_"+std::to_string(n);
            slots["items"][item]={{"name","Slot "+std::to_string(n)},{"stack",1},{"price",1},{"slot","slot"+std::to_string(n)},{"level",1}};
            slots["npcs"][1]["shop"].push_back(item);
        }
        ikv::CharacterSave equipped_save;bool equip_fail=false;int equipment_writes=0;
        ikv::World equipment(slots,[&](std::span<const ikv::CharacterSave> values){
            ++equipment_writes;if(equip_fail)throw std::runtime_error("injected equipment DB failure");
            check(values.size()==1,"one equipment character saved");equipped_save=values.front();
        });
        equipment.add(1,1,1,record(1,400));
        for(int n=1;n<=7;++n) {
            auto item="slot_item_"+std::to_string(n);
            equipment.action(1,{{"action","buy"},{"target",2147493650u},{"item",item},{"quantity",1}});
            equipment.action(1,{{"action","equip"},{"uid",uid(equipment.state(1),item.c_str())}});
        }
        check(equipment.state(1).at("character").at("state").at("equipment").size()==8,"eight admitted equipment slots");
        equipment.action(1,{{"action","buy"},{"target",2147493650u},{"item","slot_item_8"},{"quantity",1}});
        auto full=equipment.state(1);auto full_state=full.at("character").at("state");auto ninth=uid(full,"slot_item_8");int writes_before=equipment_writes;
        equipment.action(1,{{"action","equip"},{"uid",ninth}});
        auto denied=equipment.state(1);
        check(event(denied,"error","equipment") && !event(denied,"ack") && equipment_writes==writes_before,"ninth new slot rejected before persistence");
        check(denied.at("character").at("state")==full_state && equipped_save.state.at("equipment").size()==8,"ninth slot does not change cache or saved state");
        equipment.remove(1);
        equipment.add(2,1,1,{{"name","Player1"},{"class_id",1},{"state",equipped_save.state}});
        check(equipment.state(2).at("character").at("state").at("equipment").size()==8,"full equipment reconnect");
        auto old_slot=equipped_save.state.at("equipment").at("slot1").get<std::string>();
        equipment.action(2,{{"action","buy"},{"target",2147493650u},{"item","slot_item_1"},{"quantity",1}});
        auto replacement_state=equipment.state(2).at("character").at("state");std::string replacement;
        for(const auto& entry:replacement_state.at("inventory"))if(entry.at("item")=="slot_item_1" && entry.at("uid")!=old_slot)replacement=entry.at("uid");
        check(!replacement.empty(),"replacement stack exists");equip_fail=true;
        equipment.action(2,{{"action","equip"},{"uid",replacement}});
        auto rolled_back=equipment.state(2);
        check(event(rolled_back,"error","persistence") && !event(rolled_back,"ack") && rolled_back.at("character").at("state")==replacement_state,"equipment DB failure rolls back full-slot replacement");
        equip_fail=false;equipment.action(2,{{"action","equip"},{"uid",replacement}});
        auto replaced=equipment.state(2);
        check(event(replaced,"ack") && replaced.at("character").at("state").at("equipment").size()==8 &&
              replaced.at("character").at("state").at("equipment").at("slot1")==replacement,"replacement allowed at eight slots");
        equipment.remove(2);equipment.add(3,1,1,{{"name","Player1"},{"class_id",1},{"state",equipped_save.state}});
        check(equipment.state(3).at("character").at("state").at("equipment").at("slot1")==replacement,"replacement persists through reconnect");
        auto bags=config;bags["items"]["long_item"]={{"name",std::string(62,'N')},{"stack",1},{"price",1},{"level",1}};
        ikv::World full_trade(bags,[](std::span<const ikv::CharacterSave>){});
        const auto bag_record=[&](int player) {
            auto value=record(player,400);value["state"]["inventory"]=ikv::Json::array();value["state"]["equipment"]=ikv::Json::object();
            constexpr char hex[]="0123456789abcdef";
            for(uint32_t index=0;index<32;++index) {
                uint32_t number=player*32+index;std::string item_uid(32,'0');
                for(uint32_t digit=0;digit<8;++digit)item_uid[31-digit]=hex[(number>>(4*digit))&15];
                value["state"]["inventory"].push_back({{"uid",item_uid},{"item","long_item"},{"quantity",1}});
            }
            return value;
        };
        full_trade.add(1,1,1,bag_record(1));full_trade.add(2,2,2,bag_record(2));
        for(int player=3;player<=32;++player) {
            auto value=record(player,400);value["name"]=std::string(32,'"');value["appearance"]={{"p",std::string(376,'x')}};
            full_trade.add(player,player,player,value);
        }
        auto first_bag=full_trade.state(1).at("character").at("state").at("inventory");
        auto second_bag=full_trade.state(2).at("character").at("state").at("inventory");
        first_bag[0]["item"]="training_blade";first_bag[0]["name"]="forged client label";
        full_trade.action(1,{{"action","trade_request"},{"target",2}});
        auto first_invite=terms(full_trade.state(2));auto invite_revision=first_invite.at("revision").get<uint64_t>();
        full_trade.action(2,{{"action","trade_accept"},{"revision",invite_revision}});
        full_trade.action(1,{{"action","trade_offer"},{"items",first_bag},{"currency",0}});
        full_trade.action(2,{{"action","trade_offer"},{"items",second_bag},{"currency",0}});
        for(int player=3;player<=32;++player)full_trade.action(player,{{"action","chat"},{"channel","local"},{"text",std::string(512,'"')}});
        auto full_frame=full_trade.state(2);auto full_terms=terms(full_frame);
        check(full_terms.at("own").at("items").size()==32 && full_terms.at("other").at("items").size()==32 &&
              full_terms.dump().size()>8192 && full_frame.dump().size()<=65533,"full 32+32 offers survive bounded queue and wire frame");
        check(full_terms.at("peer_name")=="Player1" && full_terms.at("other").at("items")[0].at("item")=="long_item" &&
              full_terms.at("other").at("items")[0].at("name")==std::string(62,'N'),"terms resolve peer identity and owned item rather than client labels");
        const auto final_revision=full_terms.at("revision").get<uint64_t>();
        full_trade.action(1,{{"action","trade_accept"},{"revision",final_revision}});
        full_trade.action(2,{{"action","trade_accept"},{"revision",final_revision}});
        check(event(full_trade.state(2),"trade_complete") &&
              full_trade.state(1).at("character").at("state").at("inventory").size()==32,"full bag swap commits without item duplication");
        full_trade.action(1,{{"action","trade_request"},{"target",2}});
        auto next_invite=terms(full_trade.state(2));
        check(next_invite.at("revision").get<uint64_t>()>final_revision,"new exchange uses globally fresh revision");
        full_trade.action(2,{{"action","trade_accept"},{"revision",final_revision}});
        check(event(full_trade.state(2),"error","trade_revision"),"old exchange consent cannot open new exchange");
        full_trade.action(1,{{"action","trade_cancel"}});
        auto wide_name=bags;wide_name["items"]["long_item"]["name"]=std::string(63,'N');
        bool invalid_name=false;try{ikv::World rejected(wide_name,[](std::span<const ikv::CharacterSave>){});}catch(const std::invalid_argument&){invalid_name=true;}
        check(invalid_name,"catalog name envelope validated before serving trades");
        std::cout << "world transitions/economy passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

