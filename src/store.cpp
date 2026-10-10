#include "ikv/store.hpp"
#include "ikv/protocol.hpp"
#include <libpq-fe.h>
#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/params.h>
#include <openssl/rand.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <csignal>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <spawn.h>
#include <thread>
#include <unistd.h>

namespace ikv {
namespace {
using Result = std::unique_ptr<PGresult, decltype(&PQclear)>;
constexpr std::string_view process_lock = "4242427443";
Result sql(PGconn* connection, const std::string& query, std::initializer_list<std::string> args = {}) {
    std::vector<const char*> values;
    values.reserve(args.size());
    for (const auto& value : args) values.push_back(value.c_str());
    Result result(PQexecParams(connection, query.c_str(), static_cast<int>(values.size()), nullptr, values.data(), nullptr, nullptr, 0), PQclear);
    if (!result || (PQresultStatus(result.get()) != PGRES_TUPLES_OK && PQresultStatus(result.get()) != PGRES_COMMAND_OK)) {
        const char* code = result ? PQresultErrorField(result.get(), PG_DIAG_SQLSTATE) : nullptr;
        if (code && std::string_view(code) == "23505") throw ApiError(409, "conflict", "Bu isim kullanılamaz, lütfen farklı bir isim seçin.");
        throw ApiError(503, "database_error", "Veritabanı işlemi başarısız oldu; daha sonra tekrar deneyin.");
    }
    return result;
}
std::string cell(const Result& result, int row, int column) { return PQgetvalue(result.get(), row, column); }
bool boolean(const Result& result, int row, int column) { return cell(result,row,column)=="t"; }
std::int64_t integer(const Result& result, int row, int column) { return std::stoll(cell(result,row,column)); }
class Transaction {
    PGconn* connection_;
    bool committed_ = false;
public:
    explicit Transaction(PGconn* connection) : connection_(connection) { sql(connection_, "BEGIN"); }
    ~Transaction() { if (!committed_) { PGresult* r = PQexec(connection_, "ROLLBACK"); if(r) PQclear(r); } }
    void commit() { sql(connection_,"COMMIT"); committed_=true; }
};
std::string hex(std::span<const unsigned char> bytes) {
    constexpr char digits[]="0123456789abcdef";
    std::string out(bytes.size()*2,'0');
    for(std::size_t i=0;i<bytes.size();++i) { out[i*2]=digits[bytes[i]>>4]; out[i*2+1]=digits[bytes[i]&15]; }
    return out;
}
std::vector<unsigned char> unhex(std::string_view value) {
    if(value.size()%2) throw ApiError(500,"credential_error","Kayıtlı parola bilgisi bozuk.");
    std::vector<unsigned char> out(value.size()/2);
    auto nibble=[](char c)->unsigned char { if(c>='0'&&c<='9')return static_cast<unsigned char>(c-'0'); if(c>='a'&&c<='f')return static_cast<unsigned char>(c-'a'+10); throw ApiError(500,"credential_error","Kayıtlı parola bilgisi bozuk."); };
    for(std::size_t i=0;i<out.size();++i) out[i]=static_cast<unsigned char>((nibble(value[2*i])<<4)|nibble(value[2*i+1]));
    return out;
}
std::string random_hex(std::size_t size) {
    std::array<unsigned char,32> bytes{};
    if(size>bytes.size() || RAND_bytes(bytes.data(),static_cast<int>(size))!=1) throw ApiError(503,"entropy_error","Güvenli rastgele sayı üreteci kullanılamıyor.");
    return hex(std::span(bytes.data(),size));
}
std::string token_hash(std::string_view value) {
    std::array<unsigned char,32> digest{}; unsigned int length=0;
    if(EVP_Digest(value.data(),value.size(),digest.data(),&length,EVP_sha256(),nullptr)!=1 || length!=digest.size()) throw ApiError(503,"crypto_error","Özet işlevi kullanılamıyor.");
    return hex(digest);
}
std::string password_hash(std::string_view password, std::string_view salt_hex) {
    auto salt=unhex(salt_hex);
    std::unique_ptr<EVP_KDF,decltype(&EVP_KDF_free)> kdf(EVP_KDF_fetch(nullptr,"ARGON2ID",nullptr),EVP_KDF_free);
    std::unique_ptr<EVP_KDF_CTX,decltype(&EVP_KDF_CTX_free)> context(kdf ? EVP_KDF_CTX_new(kdf.get()) : nullptr,EVP_KDF_CTX_free);
    if(!context)throw ApiError(503,"crypto_error","Parola doğrulaması kullanılamıyor.");
    std::uint32_t iterations=3,lanes=1,memory=65536,threads=1;
    OSSL_PARAM params[]={
        OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_PASSWORD,const_cast<char*>(password.data()),password.size()),
        OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT,salt.data(),salt.size()),
        OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ITER,&iterations),
        OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ARGON2_LANES,&lanes),
        OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ARGON2_MEMCOST,&memory),
        OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_THREADS,&threads),OSSL_PARAM_construct_end()};
    std::array<unsigned char,32> digest{};
    if(EVP_KDF_derive(context.get(),digest.data(),digest.size(),params)!=1)throw ApiError(503,"crypto_error","Parola doğrulaması başarısız oldu.");
    auto result=hex(digest); OPENSSL_cleanse(digest.data(),digest.size()); return result;
}
// Turkish label for request fields shown to players by the launcher/client; admin-only fields keep their key.
std::string field(std::string_view key) {
    if(key=="username")return "Kullanıcı adı";
    if(key=="password"||key=="new_password")return "Parola";
    if(key=="token")return "Sıfırlama kodu";
    if(key=="name"||key=="confirm_name")return "Karakter ismi";
    if(key=="class_id")return "Sınıf";
    return std::string(key);
}
std::string text(const Json& body, const char* key, std::size_t min, std::size_t max) {
    if(!body.is_object() || !body.contains(key) || !body[key].is_string()) throw ApiError(400,"invalid_request",field(key)+" gerekli.");
    auto value=body[key].get<std::string>();
    if(value.size()<min || value.size()>max || value.find('\0')!=std::string::npos)throw ApiError(400,"invalid_request",field(key)+" "+(min==max?std::to_string(min):std::to_string(min)+"–"+std::to_string(max))+" karakter olmalı.");
    return value;
}
std::string username(const Json& body) {
    auto value=text(body,"username",3,32);
    if(!std::all_of(value.begin(),value.end(),[](char c){return (c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_';}))throw ApiError(400,"invalid_username","Kullanıcı adı yalnızca küçük İngilizce harf, rakam ve alt çizgiden oluşmalı.");
    return value;
}
std::int64_t id_value(std::string_view value) {
    if(value.empty()||value.size()>19||!std::all_of(value.begin(),value.end(),[](char c){return c>='0'&&c<='9';}))throw ApiError(400,"invalid_id","Geçersiz kimlik.");
    try {auto id=std::stoll(std::string(value));if(id>0)return id;}catch(const std::exception&){}
    throw ApiError(400,"invalid_id","Geçersiz kimlik.");
}
std::int64_t number(const Json& body,const char* key,std::int64_t min,std::int64_t max) {
    if(!body.contains(key)||!body[key].is_number_integer())throw ApiError(400,"invalid_request",field(key)+" gerekli.");
    auto value=body[key].get<std::int64_t>(); if(value<min||value>max)throw ApiError(400,"invalid_request",field(key)+" geçersiz."); return value;
}
void state_valid(const Json& state) { if(!state.is_object()||state.dump().size()>262144)throw ApiError(400,"invalid_state","Character state must be an object of at most 256 KiB"); }
constexpr const char* character_columns="c.id,c.account_id,c.name,c.class_id,c.appearance::text,c.state::text,COALESCE(m.muted_until>now(),false),COALESCE(to_char(m.muted_until AT TIME ZONE 'UTC','YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'),''),(SELECT g.state::text FROM guild_registry g JOIN guild_members gm ON gm.guild_id=g.id WHERE gm.character_id=c.id),c.bank::text";
Json character_row(const Result& result,int row) {
    auto state=Json::parse(cell(result,row,5));
    if(PQgetisnull(result.get(),row,8))state.erase("guild");else state["guild"]=Json::parse(cell(result,row,8));
    return {{"id",integer(result,row,0)},{"account_id",integer(result,row,1)},{"name",cell(result,row,2)},{"class_id",integer(result,row,3)},
        {"appearance",Json::parse(cell(result,row,4))},{"state",std::move(state)},{"bank",Json::parse(cell(result,row,9))},
        {"moderation",{{"muted",boolean(result,row,6)},{"muted_until",cell(result,row,7).empty()?Json(nullptr):Json(cell(result,row,7))}}}};
}
Identity identity(PGconn* connection,std::string_view bearer) {
    if(bearer.size()!=64||!std::all_of(bearer.begin(),bearer.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');}))throw ApiError(401,"unauthorized","Geçersiz oturum; yeniden giriş yapın.");
    auto result=sql(connection,"SELECT a.id,a.admin,a.plus FROM sessions s JOIN accounts a ON a.id=s.account_id WHERE s.token_hash=$1 AND s.revoked_at IS NULL AND s.expires_at>now() AND (a.banned_until IS NULL OR a.banned_until<=now())",{token_hash(bearer)});
    if(PQntuples(result.get())!=1)throw ApiError(401,"unauthorized","Oturumunuzun süresi doldu ya da hesabınız bloklanmış; yeniden giriş yapın.");
    return {integer(result,0,0),boolean(result,0,1),boolean(result,0,2)};
}
void audit(PGconn* connection,std::string_view action,std::int64_t actor,std::int64_t target,const Json& details=Json::object()) {
    sql(connection,"INSERT INTO audit_events(action,actor_id,target_id,details) VALUES($1,$2::bigint,NULLIF($3,'0')::bigint,$4::jsonb)",{std::string(action),std::to_string(actor),std::to_string(target),details.dump()});
}
void process(const char* binary,const std::vector<std::string>& arguments,PGconn* connection) {
    // Build child-only libpq environment; passwords never appear in argv or logs.
    std::vector<std::string> environment;
    for(char** e=::environ;*e;++e)if(!std::string_view(*e).starts_with("PG"))environment.emplace_back(*e);
    std::unique_ptr<PQconninfoOption,decltype(&PQconninfoFree)> options(PQconninfo(connection),PQconninfoFree);
    if(!options)throw ApiError(503,"backup_error","Cannot read database connection parameters");
    struct PrivateServiceFile {
        std::string path;
        PrivateServiceFile(){char pattern[]="/tmp/ikv-pg-service-XXXXXX";int fd=mkstemp(pattern);if(fd<0)throw ApiError(503,"backup_error","Cannot create private database tool configuration");path=pattern;close(fd);}
        ~PrivateServiceFile(){if(!path.empty())unlink(path.c_str());}
    } private_service;
    std::string config="[ikv_owned_tool]\n";
    auto append=[&](std::string_view key,std::string_view value){
        if(value.find_first_of("\r\n")!=std::string_view::npos)throw ApiError(503,"backup_error","Database tool options cannot contain newlines");
        config+=std::string(key)+"="+std::string(value)+"\n";
    };
    for(auto* option=options.get();option->keyword;++option){
        const std::string_view key=option->keyword;
        if(key!="service"&&key!="host"&&key!="hostaddr"&&key!="port"&&key!="dbname"&&option->val&&*option->val)append(key,option->val);
    }
    append("host",PQhost(connection));append("hostaddr",PQhostaddr(connection));append("port",PQport(connection));append("dbname",PQdb(connection));
    {
        std::ofstream stream(private_service.path,std::ios::binary|std::ios::trunc);
        stream.write(config.data(),static_cast<std::streamsize>(config.size()));stream.close();
        OPENSSL_cleanse(config.data(),config.size());
        if(!stream)throw ApiError(503,"backup_error","Cannot write private database tool configuration");
    }
    environment.emplace_back("PGSERVICEFILE="+private_service.path);
    environment.emplace_back("PGSERVICE=ikv_owned_tool");
    std::vector<char*> envp;for(auto& e:environment)envp.push_back(e.data());envp.push_back(nullptr);
    std::vector<char*> argv;argv.push_back(const_cast<char*>(binary));for(const auto& a:arguments)argv.push_back(const_cast<char*>(a.c_str()));argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    if(posix_spawn_file_actions_init(&actions)!=0)throw ApiError(503,"backup_error","Cannot prepare database tool");
    posix_spawn_file_actions_addopen(&actions,STDOUT_FILENO,"/dev/null",O_WRONLY,0600);
    posix_spawn_file_actions_addopen(&actions,STDERR_FILENO,"/dev/null",O_WRONLY,0600);
    pid_t child=0;int error=posix_spawnp(&child,binary,&actions,nullptr,argv.data(),envp.data());
    posix_spawn_file_actions_destroy(&actions);
    for(auto& e:environment)OPENSSL_cleanse(e.data(),e.size());
    if(error!=0)throw ApiError(503,"backup_error","Cannot start database tool");
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::minutes(5);
    int status=0;
    for(;;){
        auto finished=waitpid(child,&status,WNOHANG);
        if(finished==child)break;
        if(finished<0&&errno!=EINTR)throw ApiError(503,"backup_error","Cannot wait for database tool");
        if(std::chrono::steady_clock::now()>=deadline){kill(child,SIGKILL);while(waitpid(child,&status,0)<0&&errno==EINTR){}throw ApiError(503,"backup_timeout","Database tool exceeded five-minute timeout");}
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    if(!WIFEXITED(status)||WEXITSTATUS(status)!=0)throw ApiError(503,"backup_error","Database tool failed; check PostgreSQL tool versions and privileges");
}
} // anonymous namespace

struct Store::Impl {
    std::string dsn;
    std::array<PGconn*,4> connections{};
    std::array<bool,4> used{};
    std::mutex mutex;
    std::string world_instance;
    std::condition_variable available;
    struct Limit {std::chrono::steady_clock::time_point start;int count;};
    std::array<std::map<std::string,Limit>,2> limits;
    std::map<std::int64_t,std::pair<std::int64_t,std::string>> leases;
    explicit Impl(std::string value):dsn(std::move(value)) {
        const char* keywords[]={"dbname","connect_timeout","client_encoding",nullptr};
        const char* values[]={dsn.c_str(),"5","UTF8",nullptr};
        try {
            for(auto& c:connections){
                c=PQconnectdbParams(keywords,values,1);
                if(!c||PQstatus(c)!=CONNECTION_OK)throw ApiError(503,"database_error","Cannot connect to PostgreSQL");
                std::unique_ptr<PQconninfoOption,decltype(&PQconninfoFree)> options(PQconninfo(c),PQconninfoFree);
                if(!options)throw ApiError(503,"database_error","Cannot inspect database endpoint");
                for(auto* option=options.get();option->keyword;++option){
                    const std::string_view key=option->keyword;
                    if((key=="host"||key=="hostaddr"||key=="port")&&option->val&&std::string_view(option->val).find(',')!=std::string_view::npos)throw ApiError(503,"database_endpoint","A community database must use a single PostgreSQL endpoint");
                }
                if(c!=connections.front()){
                    auto* first=connections.front();
                    if(std::string_view(PQhost(c))!=PQhost(first)||std::string_view(PQhostaddr(c))!=PQhostaddr(first)||std::string_view(PQport(c))!=PQport(first)||std::string_view(PQdb(c))!=PQdb(first))throw ApiError(503,"database_endpoint","Database pool endpoints differ; use one authoritative PostgreSQL endpoint");
                }
                const int socket=PQsocket(c);
                if(socket<0||fcntl(socket,F_SETFD,FD_CLOEXEC)<0)throw ApiError(503,"database_error","Cannot secure database connection");
                sql(c,"SET statement_timeout='10s'");
                sql(c,"SET lock_timeout='5s'");
                sql(c,"SELECT pg_advisory_lock_shared($1::bigint)",{std::string(process_lock)});
            }
        }
        catch(...){for(auto* c:connections)if(c)PQfinish(c);throw;}
    }
    ~Impl(){for(auto* c:connections)PQfinish(c);OPENSSL_cleanse(dsn.data(),dsn.size());}
    struct Connection {
        Impl& owner;std::size_t index;
        explicit Connection(Impl& impl):owner(impl),index(0){std::unique_lock lock(owner.mutex);if(!owner.available.wait_for(lock,std::chrono::seconds(5),[&]{return std::any_of(owner.used.begin(),owner.used.end(),[](bool v){return !v;});}))throw ApiError(503,"busy","Sunucu meşgul; daha sonra tekrar deneyin.");while(owner.used[index])++index;owner.used[index]=true;}
        ~Connection(){std::lock_guard lock(owner.mutex);owner.used[index]=false;owner.available.notify_one();}
        operator PGconn*()const{return owner.connections[index];}
    };
    void rate(std::string key,int maximum,bool administrative=false) {
        auto now=std::chrono::steady_clock::now();std::lock_guard lock(mutex);
        auto& bucket=limits[administrative?1:0];
        for(auto it=bucket.begin();it!=bucket.end();)if(now-it->second.start>std::chrono::minutes(1))it=bucket.erase(it);else ++it;
        if(bucket.size()>=4096&&!bucket.contains(key))throw ApiError(429,"rate_limited","Çok fazla istek; biraz bekleyin.");
        auto [it,inserted]=bucket.try_emplace(std::move(key),Limit{now,0});(void)inserted;
        if(++it->second.count>maximum)throw ApiError(429,"rate_limited","Çok fazla istek; bir dakika sonra tekrar deneyin.");
    }
};
Store::Store(std::string dsn):impl_(std::make_unique<Impl>(std::move(dsn))){}
Store::~Store()=default;
void Store::migrate(const std::filesystem::path& directory) {
    if(!std::filesystem::is_directory(directory))throw ApiError(500,"migration_error","Migration directory missing");
    std::vector<std::filesystem::path> files;for(const auto& entry:std::filesystem::directory_iterator(directory))if(entry.is_regular_file()&&entry.path().extension()==".sql")files.push_back(entry.path());std::sort(files.begin(),files.end());
    if(files.empty())throw ApiError(500,"migration_error","No migrations found");
    Impl::Connection connection(*impl_);Transaction transaction(connection);
    sql(connection,"SELECT pg_advisory_xact_lock(4242427444)");
    sql(connection,"CREATE TABLE IF NOT EXISTS schema_migrations(name text PRIMARY KEY, checksum text NOT NULL, applied_at timestamptz NOT NULL DEFAULT now())");
    for(const auto& file:files){std::ifstream stream(file);std::stringstream buffer;buffer<<stream.rdbuf();if(!stream.good()&&!stream.eof())throw ApiError(500,"migration_error","Cannot read migration");auto content=buffer.str();auto checksum=token_hash(content);auto old=sql(connection,"SELECT checksum FROM schema_migrations WHERE name=$1",{file.filename().string()});if(PQntuples(old.get())){if(cell(old,0,0)!=checksum)throw ApiError(500,"migration_error","Applied migration has changed");continue;}
        Result result(PQexec(connection,content.c_str()),PQclear);if(!result||PQresultStatus(result.get())!=PGRES_COMMAND_OK)throw ApiError(500,"migration_error","SQL migration failed");sql(connection,"INSERT INTO schema_migrations(name,checksum) VALUES($1,$2)",{file.filename().string(),checksum});}
    transaction.commit();
}
Identity Store::authenticate(std::string_view bearer){Impl::Connection connection(*impl_);return identity(connection,bearer);}
Json Store::character(std::int64_t account,std::int64_t id) {
    Impl::Connection connection(*impl_);auto result=sql(connection,std::string("SELECT ")+character_columns+" FROM characters c LEFT JOIN chat_moderation m ON m.character_id=c.id WHERE c.account_id=$1::bigint AND c.id=$2::bigint AND c.deleted_at IS NULL",{std::to_string(account),std::to_string(id)});if(!PQntuples(result.get()))throw ApiError(404,"character_not_found","Character not found");return character_row(result,0);
}
Json Store::enter_character(const Identity& who,std::int64_t id,std::string_view instance) {
    if(instance.empty()||instance.size()>128)throw ApiError(400,"invalid_instance","Invalid world instance");
    {std::lock_guard guard(impl_->mutex);if(impl_->leases.contains(id))throw ApiError(409,"character_unavailable","Character is already active in this world process");}
    Impl::Connection connection(*impl_);Transaction transaction(connection);
    auto result=sql(connection,"UPDATE characters c SET lease_instance=$3,lease_expires_at=now()+interval '30 seconds' FROM accounts a WHERE c.id=$1::bigint AND c.account_id=$2::bigint AND a.id=c.account_id AND c.deleted_at IS NULL AND (a.banned_until IS NULL OR a.banned_until<=now()) AND (c.lease_expires_at IS NULL OR c.lease_expires_at<=now()) RETURNING c.id",{std::to_string(id),std::to_string(who.account_id),std::string(instance)});
    if(!PQntuples(result.get()))throw ApiError(409,"character_unavailable","Character missing, banned or already active");
    auto state=sql(connection,std::string("SELECT ")+character_columns+" FROM characters c LEFT JOIN chat_moderation m ON m.character_id=c.id WHERE c.id=$1::bigint",{std::to_string(id)});auto out=character_row(state,0);transaction.commit();
    {std::lock_guard lock(impl_->mutex);impl_->leases[id]={who.account_id,std::string(instance)};}return out;
}
void Store::leave_character(std::int64_t account,std::int64_t id) {
    std::string instance;{std::lock_guard lock(impl_->mutex);auto it=impl_->leases.find(id);if(it==impl_->leases.end()||it->second.first!=account)return;instance=it->second.second;}
    Impl::Connection connection(*impl_);sql(connection,"UPDATE characters SET lease_instance=NULL,lease_expires_at=NULL WHERE id=$1::bigint AND account_id=$2::bigint AND lease_instance=$3",{std::to_string(id),std::to_string(account),instance});
    std::lock_guard lock(impl_->mutex);impl_->leases.erase(id);
}
void Store::save_character(std::int64_t account,std::int64_t id,const Json& state){CharacterSave save{account,id,state};save_characters(std::span(&save,1));}
void Store::save_characters(std::span<const CharacterSave> saves) {
    if(saves.empty()||saves.size()>32)throw ApiError(400,"invalid_save","Invalid save batch");
    std::vector<const CharacterSave*> ordered;ordered.reserve(saves.size());for(const auto& save:saves){state_valid(save.state);ordered.push_back(&save);}std::sort(ordered.begin(),ordered.end(),[](auto a,auto b){return a->character_id<b->character_id;});
    Impl::Connection connection(*impl_);Transaction transaction(connection);std::int64_t previous=0;
    // ponytail: one transaction lock for guild roster writes; partition by guild if communities outgrow 32 live players.
    if(std::any_of(ordered.begin(),ordered.end(),[](const CharacterSave* save){return save->state.contains("guild")&&!save->state["guild"].is_null();}))sql(connection,"SELECT pg_advisory_xact_lock(4242427446)");
    for(const auto* save:ordered){if(save->character_id<=0||save->character_id==previous)throw ApiError(400,"invalid_save","Duplicate character in batch");previous=save->character_id;
        std::string instance;{std::lock_guard lock(impl_->mutex);auto it=impl_->leases.find(save->character_id);if(it==impl_->leases.end()||it->second.first!=save->account_id)throw ApiError(409,"lease_lost","Character is not owned by this world process");instance=it->second.second;}
        auto live=sql(connection,"SELECT c.id FROM characters c WHERE c.id=$1::bigint AND c.account_id=$2::bigint AND c.deleted_at IS NULL AND c.lease_instance=$3 AND c.lease_expires_at>now() FOR UPDATE OF c",{std::to_string(save->character_id),std::to_string(save->account_id),instance});if(!PQntuples(live.get()))throw ApiError(409,"lease_lost","Character lease expired or ownership changed");
        // The bank shares the state row update, so an item/currency move between them commits atomically or not at all.
        if(!save->bank.is_null()){if(!save->bank.is_object()||save->bank.dump().size()>65536)throw ApiError(400,"invalid_bank","Bank must be an object of at most 64 KiB");
            sql(connection,"UPDATE characters SET state=$2::jsonb,bank=$3::jsonb,updated_at=now() WHERE id=$1::bigint",{std::to_string(save->character_id),save->state.dump(),save->bank.dump()});}
        else sql(connection,"UPDATE characters SET state=$2::jsonb,updated_at=now() WHERE id=$1::bigint",{std::to_string(save->character_id),save->state.dump()});}
    std::map<std::string,Json> guilds;
    for(const auto* save:ordered)if(save->state.contains("guild")&&!save->state["guild"].is_null()){
        const auto& guild=save->state["guild"];auto id=text(guild,"id",1,128);text(guild,"name",2,32);
        number(guild,"revision",1,INT64_MAX);auto leader=number(guild,"leader_character_id",0,INT64_MAX);
        if(!guild.contains("members")||!guild["members"].is_array()||guild["members"].size()>256)throw ApiError(400,"invalid_guild","Guild roster must be an array of at most 256 characters");
        std::set<std::int64_t> members;for(const auto& member:guild["members"]){if(!member.is_number_integer())throw ApiError(400,"invalid_guild","Invalid guild member");auto id_value=member.get<std::int64_t>();if(id_value<=0||!members.insert(id_value).second)throw ApiError(400,"invalid_guild","Invalid or duplicate guild member");}
        if((members.empty()&&leader!=0)||(!members.empty()&&!members.contains(leader)))throw ApiError(400,"invalid_guild","Guild leader must be a member");
        auto [it,inserted]=guilds.emplace(id,guild);if(!inserted&&it->second!=guild)throw ApiError(409,"guild_conflict","Save batch disagrees on guild roster");
    }
    if(!guilds.empty()){
        for(const auto& [id,guild]:guilds){
            auto old=sql(connection,"SELECT revision,state::text FROM guild_registry WHERE id=$1 FOR UPDATE",{id});
            auto revision=guild["revision"].get<std::int64_t>();
            if(PQntuples(old.get())){
                auto old_revision=integer(old,0,0);auto old_state=Json::parse(cell(old,0,1));
                if(revision==old_revision&&old_state==guild)continue;
                if(old_revision==INT64_MAX||revision!=old_revision+1)throw ApiError(409,"guild_revision","Guild roster revision is stale");
            }else if(revision!=1)throw ApiError(409,"guild_revision","New guild must start at revision one");
            auto valid=sql(connection,"SELECT count(*) FROM (SELECT id FROM characters WHERE deleted_at IS NULL AND id IN (SELECT value::bigint FROM jsonb_array_elements_text($1::jsonb)) ORDER BY id FOR KEY SHARE) valid",{guild["members"].dump()});
            if(integer(valid,0,0)!=static_cast<std::int64_t>(guild["members"].size()))throw ApiError(409,"guild_member_missing","Guild roster contains a missing character");
            sql(connection,"INSERT INTO guild_registry(id,revision,state) VALUES($1,$2::bigint,$3::jsonb) ON CONFLICT(id) DO UPDATE SET revision=EXCLUDED.revision,state=EXCLUDED.state,updated_at=now()",{id,std::to_string(revision),guild.dump()});
            sql(connection,"DELETE FROM guild_members WHERE guild_id=$1",{id});
            sql(connection,"INSERT INTO guild_members(character_id,guild_id) SELECT value::bigint,$2 FROM jsonb_array_elements_text($1::jsonb)",{guild["members"].dump(),id});
        }
    }
    transaction.commit();
}
void Store::world_status(const Json& status) {
    auto instance=text(status,"instance",1,128);if(status.dump().size()>65536)throw ApiError(400,"invalid_status","World status too large");
    Impl::Connection connection(*impl_);Transaction transaction(connection);
    {
        std::lock_guard guard(impl_->mutex);
        if(impl_->world_instance.empty()){
            auto claim=sql(connection,"SELECT pg_try_advisory_lock(4242427445)");
            if(!boolean(claim,0,0))throw ApiError(409,"world_already_running","Only one authoritative world process is allowed per community");
            impl_->world_instance=instance;
        } else if(impl_->world_instance!=instance)throw ApiError(409,"world_instance_mismatch","A world Store cannot publish another instance");
    }
    sql(connection,"INSERT INTO world_instances(instance,status) VALUES($1,$2::jsonb) ON CONFLICT(instance) DO UPDATE SET status=EXCLUDED.status,updated_at=now()",{instance,status.dump()});
    auto renewed=sql(connection,"UPDATE characters SET lease_expires_at=now()+interval '30 seconds' WHERE lease_instance=$1 AND lease_expires_at>now() AND deleted_at IS NULL RETURNING id,account_id",{instance});
    {
        std::lock_guard guard(impl_->mutex);
        std::set<std::int64_t> live;
        for(int i=0;i<PQntuples(renewed.get());++i){auto id=integer(renewed,i,0);auto lease=impl_->leases.find(id);if(lease!=impl_->leases.end()&&lease->second.first==integer(renewed,i,1)&&lease->second.second==instance)live.insert(id);}
        for(const auto& [id,lease]:impl_->leases)if(lease.second==instance&&!live.contains(id))throw ApiError(409,"lease_lost","A character lease expired; stop the world before serving further actions");
    }
    transaction.commit();
}
Json Store::take_world_commands(){
    Impl::Connection connection(*impl_);Transaction transaction(connection);
    auto result=sql(connection,"UPDATE world_commands SET consumed_at=now() WHERE id IN (SELECT id FROM world_commands WHERE consumed_at IS NULL ORDER BY id LIMIT 16 FOR UPDATE SKIP LOCKED) RETURNING id,command,arguments::text");
    transaction.commit();Json rows=Json::array();
    for(int i=0;i<PQntuples(result.get());++i)rows.push_back({{"id",integer(result,i,0)},{"command",cell(result,i,1)},{"arguments",Json::parse(cell(result,i,2))}});
    std::sort(rows.begin(),rows.end(),[](const Json& a,const Json& b){return a.at("id").get<std::int64_t>()<b.at("id").get<std::int64_t>();});
    return rows;
}
void Store::finish_world_command(std::int64_t id,const Json& result){
    auto value=result.dump(-1,' ',false,Json::error_handler_t::replace);if(value.size()>16384)value=R"({"ok":false,"error":"Result exceeded 16 KiB"})";
    Impl::Connection connection(*impl_);sql(connection,"UPDATE world_commands SET result=$2::jsonb WHERE id=$1::bigint",{std::to_string(id),value});
}
Json Store::world_settings(){Impl::Connection connection(*impl_);auto result=sql(connection,"SELECT settings::text FROM world_settings WHERE id=1");return PQntuples(result.get())?Json::parse(cell(result,0,0)):Json::object();}
std::string Store::backup_download(std::string_view bearer,std::string_view name,std::string_view peer){
    auto who=authenticate(bearer);impl_->rate("account:"+std::to_string(who.account_id),600,peer.starts_with("admin:"));
    if(!who.admin)throw ApiError(403,"forbidden","Administrator role required");
    impl_->rate("backup_download",6,true);
    if(name.size()<9||name.size()>64||!name.starts_with("ikv-")||!name.ends_with(".sql")||!std::all_of(name.begin()+4,name.end()-4,[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f')||c=='-';}))throw ApiError(404,"backup_not_found","Backup not found");
    const char* configured=std::getenv("IKV_BACKUP_DIR");const auto file=std::filesystem::path(configured?configured:"backups")/std::string(name);
    constexpr off_t limit=256LL*1024*1024;
    const int fd=open(file.c_str(),O_RDONLY|O_NOFOLLOW|O_CLOEXEC);if(fd<0)throw ApiError(404,"backup_not_found","Backup not found");
    struct Closer{int fd;~Closer(){close(fd);}} closer{fd};
    struct stat info{};if(fstat(fd,&info)!=0||!S_ISREG(info.st_mode)||info.st_uid!=geteuid())throw ApiError(404,"backup_not_found","Backup not found");
    if(info.st_size>limit)throw ApiError(413,"backup_too_large","Backups above 256 MiB must be copied from the server's backup directory");
    std::string bytes(static_cast<std::size_t>(info.st_size),'\0');std::size_t done=0;
    while(done<bytes.size()){auto n=read(fd,bytes.data()+done,bytes.size()-done);if(n<0&&errno==EINTR)continue;if(n<=0)throw ApiError(503,"backup_error","Cannot read backup");done+=static_cast<std::size_t>(n);}
    Impl::Connection connection(*impl_);audit(connection,"database.backup_download",who.account_id,0,{{"name",std::string(name)}});
    return bytes;
}
void Store::bootstrap_admin(std::string_view user){Impl::Connection connection(*impl_);Transaction transaction(connection);auto result=sql(connection,"UPDATE accounts SET admin=true WHERE username=$1 RETURNING id",{std::string(user)});if(!PQntuples(result.get()))throw ApiError(404,"account_not_found","Bootstrap account must already exist");audit(connection,"admin.bootstrap",integer(result,0,0),integer(result,0,0));transaction.commit();}

Json Store::api(std::string_view method,std::string_view raw_path,std::string_view bearer,const Json& body,std::string_view peer) {
    const auto query_at=raw_path.find('?');const auto path=raw_path.substr(0,query_at);
    if(raw_path.size()>2048 || peer.size()>128)throw ApiError(400,"invalid_request","İstek adresi çok uzun.");
    if(query_at!=std::string_view::npos&&path!="/v1/admin/accounts"&&path!="/v1/admin/characters")throw ApiError(400,"invalid_query","Bu adreste sorgu parametresi kullanılamaz.");
    if(method=="GET"&&path=="/v1/health"){Impl::Connection connection(*impl_);sql(connection,"SELECT 1");return {{"ok",true}};}
    if(method=="GET"&&path=="/v1/status"){
        Impl::Connection connection(*impl_);auto result=sql(connection,"SELECT instance,status::text FROM world_instances WHERE updated_at>now()-interval '5 seconds' ORDER BY updated_at DESC LIMIT 1");
        Json world=Json::object();if(PQntuples(result.get())){auto state=Json::parse(cell(result,0,1));world["instance"]=cell(result,0,0);if(state.contains("players"))world["player_count"]=state["players"];else if(state.contains("player_count"))world["player_count"]=state["player_count"];if(state.contains("tick"))world["tick"]=state["tick"];if(state.contains("world_port"))world["world_port"]=state["world_port"];}return {{"online",PQntuples(result.get())>0},{"world",world}};
    }
    if(method=="POST"&&(path=="/v1/register"||path=="/v1/login"||path=="/v1/reset")){
        impl_->rate("peer:"+std::string(peer),60,peer.starts_with("admin:"));auto user=username(body);
        const auto credential_limit="credentials:"+std::string(path)+":"+std::string(peer)+":"+user;
        if(path=="/v1/register"){
            if(body.contains("admin")||body.contains("role"))throw ApiError(400,"invalid_request","Kayıt sırasında yetki verilemez.");
            auto password=text(body,"password",12,256);impl_->rate(credential_limit,12,peer.starts_with("admin:"));
            auto salt=random_hex(16);auto hash=password_hash(password,salt);OPENSSL_cleanse(password.data(),password.size());
            Impl::Connection connection(*impl_);Transaction transaction(connection);auto result=sql(connection,"INSERT INTO accounts(username,password_salt,password_hash) VALUES($1,$2,$3) RETURNING id",{user,salt,hash});auto id=integer(result,0,0);audit(connection,"account.register",id,id);transaction.commit();return {{"account_id",id}};
        }
        if(path=="/v1/login"){
            auto password=text(body,"password",1,256);impl_->rate(credential_limit,12,peer.starts_with("admin:"));
            Impl::Connection connection(*impl_);Transaction transaction(connection);
            auto result=sql(connection,"SELECT id,password_salt,password_hash,COALESCE(banned_until>now(),false) FROM accounts WHERE username=$1 FOR UPDATE",{user});
            // Equal KDF work for nonexistent accounts avoids a cheap username oracle.
            auto salt=PQntuples(result.get())?cell(result,0,1):std::string(32,'0');
            auto hash=password_hash(password,salt);OPENSSL_cleanse(password.data(),password.size());
            if(!PQntuples(result.get())||cell(result,0,2).size()!=hash.size()||CRYPTO_memcmp(cell(result,0,2).data(),hash.data(),hash.size())!=0||boolean(result,0,3))throw ApiError(401,"unauthorized","Kullanıcı isminiz ya da şifreniz onaylanmadı ya da hesabınız bloklanmış.");
            const auto id=integer(result,0,0);auto token=random_hex(32);
            sql(connection,"DELETE FROM sessions WHERE account_id=$1::bigint AND (expires_at<=now() OR revoked_at IS NOT NULL)",{std::to_string(id)});
            auto count=sql(connection,"SELECT count(*) FROM sessions WHERE account_id=$1::bigint AND revoked_at IS NULL AND expires_at>now()",{std::to_string(id)});
            if(integer(count,0,0)>=8)throw ApiError(409,"session_limit","Açık oturum sınırına ulaşıldı; önce bir oturumu kapatın.");
            auto expires=sql(connection,"INSERT INTO sessions(token_hash,account_id,expires_at) VALUES($1,$2::bigint,now()+interval '12 hours') RETURNING to_char(expires_at AT TIME ZONE 'UTC','YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"')",{token_hash(token),std::to_string(id)});
            audit(connection,"session.login",id,id);transaction.commit();return {{"session",token},{"expires_at",cell(expires,0,0)}};
        }
        auto token=text(body,"token",64,64);auto password=text(body,"new_password",12,256);
        if(!std::all_of(token.begin(),token.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');}))throw ApiError(400,"invalid_reset","Geçersiz sıfırlama kodu.");
        impl_->rate(credential_limit,12,peer.starts_with("admin:"));auto salt=random_hex(16);auto hash=password_hash(password,salt);OPENSSL_cleanse(password.data(),password.size());
        Impl::Connection connection(*impl_);Transaction transaction(connection);
        auto account=sql(connection,"SELECT id FROM accounts WHERE username=$1 FOR UPDATE",{user});
        if(!PQntuples(account.get()))throw ApiError(400,"invalid_reset","Sıfırlama kodu geçersiz ya da süresi dolmuş.");
        auto id=integer(account,0,0);auto reset=sql(connection,"UPDATE password_resets SET consumed_at=now() WHERE token_hash=$1 AND account_id=$2::bigint AND consumed_at IS NULL AND expires_at>now() RETURNING account_id",{token_hash(token),std::to_string(id)});
        if(!PQntuples(reset.get()))throw ApiError(400,"invalid_reset","Sıfırlama kodu geçersiz ya da süresi dolmuş.");
        sql(connection,"UPDATE accounts SET password_salt=$2,password_hash=$3 WHERE id=$1::bigint",{std::to_string(id),salt,hash});
        sql(connection,"UPDATE sessions SET revoked_at=now() WHERE account_id=$1::bigint AND revoked_at IS NULL",{std::to_string(id)});
        sql(connection,"UPDATE password_resets SET consumed_at=now() WHERE account_id=$1::bigint AND consumed_at IS NULL",{std::to_string(id)});
        audit(connection,"password.reset",id,id);transaction.commit();return {{"ok",true}};
    }
    auto who=authenticate(bearer);impl_->rate("account:"+std::to_string(who.account_id),600,peer.starts_with("admin:"));
    if(method=="POST"&&path=="/v1/logout"){Impl::Connection connection(*impl_);Transaction transaction(connection);sql(connection,"UPDATE sessions SET revoked_at=now() WHERE token_hash=$1",{token_hash(bearer)});audit(connection,"session.logout",who.account_id,who.account_id);transaction.commit();return {{"ok",true}};}
    if(method=="GET"&&path=="/v1/characters"){
        Impl::Connection connection(*impl_);auto result=sql(connection,std::string("SELECT ")+character_columns+" FROM characters c LEFT JOIN chat_moderation m ON m.character_id=c.id WHERE c.account_id=$1::bigint AND c.deleted_at IS NULL ORDER BY c.id",{std::to_string(who.account_id)});Json rows=Json::array();for(int i=0;i<PQntuples(result.get());++i)rows.push_back(character_row(result,i));return {{"characters",rows}};
    }
    if(method=="POST"&&path=="/v1/characters"){
        auto name=text(body,"name",2,32);if(std::any_of(name.begin(),name.end(),[](unsigned char c){return c<32||c==127;}))throw ApiError(400,"invalid_name","Karakter ismi geçersiz karakter içeriyor.");
        auto class_id=number(body,"class_id",1,3);auto appearance=body.value("appearance",Json::object());
        if(!valid_appearance(appearance)||body.contains("state"))throw ApiError(400,"invalid_request","Geçersiz karakter görünümü.");
        Impl::Connection connection(*impl_);Transaction transaction(connection);sql(connection,"SELECT id FROM accounts WHERE id=$1::bigint FOR UPDATE",{std::to_string(who.account_id)});
        auto count=sql(connection,"SELECT count(*) FROM characters WHERE account_id=$1::bigint AND deleted_at IS NULL",{std::to_string(who.account_id)});if(integer(count,0,0)>=8)throw ApiError(409,"character_limit","En fazla 8 karakteriniz olabilir.");
        auto result=sql(connection,"INSERT INTO characters(account_id,name,class_id,appearance) VALUES($1::bigint,$2,$3::integer,$4::jsonb) RETURNING id",{std::to_string(who.account_id),name,std::to_string(class_id),appearance.dump()});auto id=integer(result,0,0);
        auto record=sql(connection,std::string("SELECT ")+character_columns+" FROM characters c LEFT JOIN chat_moderation m ON m.character_id=c.id WHERE c.id=$1::bigint",{std::to_string(id)});auto response=character_row(record,0);audit(connection,"character.create",who.account_id,id);transaction.commit();return response;
    }
    if(method=="DELETE"&&path.starts_with("/v1/characters/")){
        auto id=id_value(path.substr(std::string_view("/v1/characters/").size()));auto confirm=text(body,"confirm_name",2,32);Impl::Connection connection(*impl_);Transaction transaction(connection);
        auto result=sql(connection,"SELECT name,COALESCE(lease_expires_at>now(),false) FROM characters WHERE id=$1::bigint AND account_id=$2::bigint AND deleted_at IS NULL FOR UPDATE",{std::to_string(id),std::to_string(who.account_id)});
        if(!PQntuples(result.get()))
            throw ApiError(404,"character_not_found","Karakter bulunamadı.");
        if(cell(result,0,0)!=confirm)
            throw ApiError(400,"confirmation_mismatch","Onay için yazılan isim karakterin ismiyle aynı değil.");
        if(boolean(result,0,1))
            throw ApiError(409,"character_active","Karakteri silmeden önce oyundan çıkın.");
        auto membership=sql(connection,"SELECT guild_id FROM guild_members WHERE character_id=$1::bigint",{std::to_string(id)});if(PQntuples(membership.get()))throw ApiError(409,"character_in_guild","Karakteri silmeden önce loncadan ayrılın.");
        sql(connection,"UPDATE characters SET deleted_at=now(),lease_instance=NULL,lease_expires_at=NULL WHERE id=$1::bigint",{std::to_string(id)});audit(connection,"character.delete",who.account_id,id);transaction.commit();return {{"ok",true}};
    }
    if(!path.starts_with("/v1/admin/"))throw ApiError(404,"not_found","Böyle bir adres yok.");
    if(!who.admin)throw ApiError(403,"forbidden","Administrator role required");
    if(method=="GET"&&path=="/v1/admin/status"){
        Impl::Connection connection(*impl_);auto counts=sql(connection,"SELECT (SELECT count(*) FROM accounts),(SELECT count(*) FROM characters WHERE deleted_at IS NULL),(SELECT count(*) FROM sessions WHERE expires_at>now() AND revoked_at IS NULL)");
        Json worlds=Json::array();auto rows=sql(connection,"SELECT instance,status::text,to_char(updated_at AT TIME ZONE 'UTC','YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'),updated_at>now()-interval '5 seconds' FROM world_instances ORDER BY instance");
        for(int i=0;i<PQntuples(rows.get());++i)worlds.push_back({{"instance",cell(rows,i,0)},{"status",Json::parse(cell(rows,i,1))},{"updated_at",cell(rows,i,2)},{"online",boolean(rows,i,3)}});
        Json logs=Json::array();rows=sql(connection,"SELECT id,action,COALESCE(actor_id,0),COALESCE(target_id,0),details::text,to_char(created_at AT TIME ZONE 'UTC','YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') FROM audit_events ORDER BY id DESC LIMIT 100");
        for(int i=0;i<PQntuples(rows.get());++i)logs.push_back({{"id",integer(rows,i,0)},{"action",cell(rows,i,1)},{"actor_id",integer(rows,i,2)},{"target_id",integer(rows,i,3)},{"details",Json::parse(cell(rows,i,4))},{"created_at",cell(rows,i,5)}});
        Json commands=Json::array();rows=sql(connection,"SELECT id,command,arguments::text,COALESCE(result::text,'null'),actor_id,to_char(created_at AT TIME ZONE 'UTC','YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'),consumed_at IS NOT NULL FROM world_commands ORDER BY id DESC LIMIT 30");
        for(int i=0;i<PQntuples(rows.get());++i)commands.push_back({{"id",integer(rows,i,0)},{"command",cell(rows,i,1)},{"arguments",Json::parse(cell(rows,i,2))},{"result",Json::parse(cell(rows,i,3))},{"actor_id",integer(rows,i,4)},{"created_at",cell(rows,i,5)},{"consumed",boolean(rows,i,6)}});
        return {{"accounts",integer(counts,0,0)},{"characters",integer(counts,0,1)},{"sessions",integer(counts,0,2)},{"worlds",worlds},{"logs",logs},{"commands",commands}};
    }
    if(method=="GET"&&(path=="/v1/admin/accounts"||path=="/v1/admin/characters")){
        std::string search;
        if(query_at!=std::string_view::npos){auto query=raw_path.substr(query_at+1);if(!query.starts_with("search="))throw ApiError(400,"invalid_query","Only search query is supported");auto encoded=query.substr(7);for(std::size_t i=0;i<encoded.size();++i){if(encoded[i]=='%'){if(i+2>=encoded.size())throw ApiError(400,"invalid_query","Invalid percent escape");auto digits=std::string(encoded.substr(i+1,2));std::transform(digits.begin(),digits.end(),digits.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});try{search.push_back(static_cast<char>(unhex(digits)[0]));}catch(...){throw ApiError(400,"invalid_query","Invalid percent escape");}i+=2;}else search.push_back(encoded[i]=='+'?' ':encoded[i]);}if(search.size()>64||search.find('\0')!=std::string::npos)throw ApiError(400,"invalid_query","Invalid search");}
        Impl::Connection connection(*impl_);Json rows=Json::array();
        if(path=="/v1/admin/accounts"){auto result=sql(connection,"SELECT id,username,admin,COALESCE(banned_until>now(),false),ban_reason,plus FROM accounts WHERE strpos(lower(username),lower($1))>0 ORDER BY id LIMIT 100",{search});for(int i=0;i<PQntuples(result.get());++i)rows.push_back({{"id",integer(result,i,0)},{"username",cell(result,i,1)},{"admin",boolean(result,i,2)},{"banned",boolean(result,i,3)},{"ban_reason",cell(result,i,4)},{"plus",boolean(result,i,5)}});return {{"accounts",rows}};}
        auto result=sql(connection,std::string("SELECT ")+character_columns+" FROM characters c LEFT JOIN chat_moderation m ON m.character_id=c.id WHERE c.deleted_at IS NULL AND strpos(lower(c.name),lower($1))>0 ORDER BY c.id LIMIT 100",{search});for(int i=0;i<PQntuples(result.get());++i)rows.push_back(character_row(result,i));return {{"characters",rows}};
    }
    if(method=="POST"&&path.starts_with("/v1/admin/accounts/")){
        auto rest=path.substr(std::string_view("/v1/admin/accounts/").size());auto slash=rest.find('/');if(slash==std::string_view::npos)throw ApiError(404,"not_found","Route not found");auto id=id_value(rest.substr(0,slash));auto action=rest.substr(slash+1);
        if(action!="ban"&&action!="unban"&&action!="reset"&&action!="plus"&&action!="unplus")throw ApiError(404,"not_found","Route not found");
        if(id==who.account_id&&action=="ban")throw ApiError(409,"self_ban","Cannot ban the active administrator");
        Impl::Connection connection(*impl_);Transaction transaction(connection);auto account=sql(connection,"SELECT id FROM accounts WHERE id=$1::bigint FOR UPDATE",{std::to_string(id)});if(!PQntuples(account.get()))throw ApiError(404,"account_not_found","Account not found");
        if(action=="ban"){auto reason=text(body,"reason",1,512);auto seconds=number(body,"duration_seconds",0,31536000);sql(connection,"UPDATE accounts SET banned_until=CASE WHEN $2::bigint=0 THEN 'infinity'::timestamptz ELSE now()+$2::bigint*interval '1 second' END,ban_reason=$3 WHERE id=$1::bigint",{std::to_string(id),std::to_string(seconds),reason});sql(connection,"UPDATE sessions SET revoked_at=now() WHERE account_id=$1::bigint AND revoked_at IS NULL",{std::to_string(id)});audit(connection,"account.ban",who.account_id,id,{{"reason",reason},{"duration_seconds",seconds}});}
        if(action=="unban"){sql(connection,"UPDATE accounts SET banned_until=NULL,ban_reason='' WHERE id=$1::bigint",{std::to_string(id)});audit(connection,"account.unban",who.account_id,id);}
        // Plus Paket (migration 005): the world re-reads it at its 1 s session heartbeat, so a connected player is updated live.
        if(action=="plus"||action=="unplus"){sql(connection,"UPDATE accounts SET plus=$2::boolean WHERE id=$1::bigint",{std::to_string(id),action=="plus"?"true":"false"});audit(connection,"account."+std::string(action),who.account_id,id);}
        if(action=="reset"){auto token=random_hex(32);sql(connection,"UPDATE password_resets SET consumed_at=now() WHERE account_id=$1::bigint AND consumed_at IS NULL",{std::to_string(id)});auto expires=sql(connection,"INSERT INTO password_resets(token_hash,account_id,expires_at) VALUES($1,$2::bigint,now()+interval '15 minutes') RETURNING to_char(expires_at AT TIME ZONE 'UTC','YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"')",{token_hash(token),std::to_string(id)});audit(connection,"password.reset_issued",who.account_id,id);transaction.commit();return {{"token",token},{"expires_at",cell(expires,0,0)}};}
        transaction.commit();return {{"ok",true}};
    }
    if(method=="POST"&&path=="/v1/admin/moderation"){
        auto id=number(body,"character_id",1,INT64_MAX);auto action=text(body,"action",4,6);if(action!="mute"&&action!="unmute")throw ApiError(400,"invalid_action","Use mute or unmute");
        Impl::Connection connection(*impl_);Transaction transaction(connection);auto character=sql(connection,"SELECT id FROM characters WHERE id=$1::bigint AND deleted_at IS NULL FOR UPDATE",{std::to_string(id)});if(!PQntuples(character.get()))throw ApiError(404,"character_not_found","Character not found");
        if(action=="mute"){auto seconds=number(body,"duration_seconds",1,604800);sql(connection,"INSERT INTO chat_moderation(character_id,muted_until,actor_id) VALUES($1::bigint,now()+$2::bigint*interval '1 second',$3::bigint) ON CONFLICT(character_id) DO UPDATE SET muted_until=EXCLUDED.muted_until,actor_id=EXCLUDED.actor_id",{std::to_string(id),std::to_string(seconds),std::to_string(who.account_id)});}
        else sql(connection,"DELETE FROM chat_moderation WHERE character_id=$1::bigint",{std::to_string(id)});
        audit(connection,"character."+action,who.account_id,id);transaction.commit();return {{"ok",true}};
    }
    // World commands travel through world_commands; the world consumes them at its 1 s heartbeat and records a result.
    const auto enqueue=[&](const std::string& command,const Json& arguments)->Json{
        Impl::Connection connection(*impl_);Transaction transaction(connection);
        auto pending=sql(connection,"SELECT count(*),count(*) FILTER (WHERE command IN ('restart','shutdown')) FROM world_commands WHERE consumed_at IS NULL");
        if(integer(pending,0,0)>=64)throw ApiError(429,"command_queue_full","64 world commands are already pending; check that the world is running");
        if((command=="restart"||command=="shutdown")&&integer(pending,0,1))throw ApiError(409,"command_pending","A world control command is pending");
        auto id=sql(connection,"INSERT INTO world_commands(command,arguments,actor_id) VALUES($1,$2::jsonb,$3::bigint) RETURNING id",{command,arguments.dump(),std::to_string(who.account_id)});
        audit(connection,"world."+command,who.account_id,0,arguments);transaction.commit();return {{"ok",true},{"command_id",integer(id,0,0)}};
    };
    const auto line=[&](const Json& source,const char* key,std::size_t min,std::size_t max){
        auto value=text(source,key,min,max);
        if(std::any_of(value.begin(),value.end(),[](unsigned char c){return c<32||c==127;}))throw ApiError(400,"invalid_request",std::string("Control characters in ")+key);
        return value;
    };
    if(method=="POST"&&path=="/v1/admin/control"){
        auto command=text(body,"command",6,8);if(command!="restart"&&command!="shutdown"&&command!="cancel")throw ApiError(400,"invalid_command","Use restart, shutdown or cancel");
        Json arguments=Json::object();
        if(command!="cancel"){
            arguments["delay_seconds"]=body.contains("delay_seconds")?number(body,"delay_seconds",0,3600):0;
            if(body.contains("message"))arguments["message"]=line(body,"message",0,256);
        }
        return enqueue(command,arguments);
    }
    if(method=="POST"&&path=="/v1/admin/world"){
        const auto command=text(body,"command",4,15);Json arguments=Json::object();
        if(command=="announce")arguments["message"]=line(body,"message",1,256);
        else if(command=="kick"){arguments["character_id"]=number(body,"character_id",1,INT64_MAX);if(body.contains("reason"))arguments["reason"]=line(body,"reason",0,256);}
        else if(command=="teleport"){
            arguments["character_id"]=number(body,"character_id",1,INT64_MAX);arguments["zone"]=line(body,"zone",1,32);
            if(body.contains("x")||body.contains("y")){arguments["x"]=number(body,"x",-10000000,10000000);arguments["y"]=number(body,"y",-10000000,10000000);}
        }
        else if(command=="summon"){arguments["character_id"]=number(body,"character_id",1,INT64_MAX);arguments["to_character_id"]=number(body,"to_character_id",1,INT64_MAX);}
        else if(command=="clock")arguments["hour"]=number(body,"hour",0,23);
        else if(command=="respawn"){if(body.contains("zone"))arguments["zone"]=line(body,"zone",1,32);}
        else if(command!="validate_config")throw ApiError(400,"invalid_command","Use announce, kick, teleport, summon, clock, respawn or validate_config");
        return enqueue(command,arguments);
    }
    if(method=="POST"&&path=="/v1/admin/bots"){
        const auto action=text(body,"action",5,7);Json arguments=Json::object();
        if(action=="spawn"){
            arguments["count"]=number(body,"count",1,50);arguments["behaviour"]=text(body,"behaviour",4,9);
            if(arguments["behaviour"]!="town"&&arguments["behaviour"]!="hunt"&&arguments["behaviour"]!="companion")throw ApiError(400,"invalid_request","Behaviour is town, hunt or companion");
            if(body.contains("zone"))arguments["zone"]=line(body,"zone",1,32);
            if(body.contains("level"))arguments["level"]=number(body,"level",1,100);
            return enqueue("bots_spawn",arguments);
        }
        if(action!="despawn")throw ApiError(400,"invalid_request","Action is spawn or despawn");
        if(body.contains("all")&&body["all"]==true)arguments["all"]=true;
        else if(body.contains("zone"))arguments["zone"]=line(body,"zone",1,32);
        else {
            if(!body.contains("ids")||!body["ids"].is_array()||body["ids"].empty()||body["ids"].size()>64)throw ApiError(400,"invalid_request","Despawn needs all:true, a zone or 1-64 bot ids");
            for(const auto& id:body["ids"])if(!id.is_number_integer()||id.get<std::int64_t>()<1||id.get<std::int64_t>()>INT32_MAX)throw ApiError(400,"invalid_request","Invalid bot id");
            arguments["ids"]=body["ids"];
        }
        return enqueue("bots_despawn",arguments);
    }
    if(path=="/v1/admin/settings"){
        if(method=="GET")return {{"settings",world_settings()}};
        if(method!="POST")throw ApiError(404,"not_found","Route not found");
        Json settings=Json::object();
        for(const char* key:{"xp_rate","drop_rate","regen_rate"})settings[key]=number(body,key,10,1000);
        if(!body.contains("disabled_zones")||!body["disabled_zones"].is_array()||body["disabled_zones"].size()>128)throw ApiError(400,"invalid_request","disabled_zones must be an array of at most 128 zone names");
        std::set<std::string> zones;for(const auto& zone:body["disabled_zones"]){Json wrapper{{"zone",zone}};zones.insert(line(wrapper,"zone",1,32));}
        settings["disabled_zones"]=zones;
        {Impl::Connection connection(*impl_);sql(connection,"INSERT INTO world_settings(id,settings,actor_id) VALUES(1,$1::jsonb,$2::bigint) ON CONFLICT(id) DO UPDATE SET settings=EXCLUDED.settings,actor_id=EXCLUDED.actor_id,updated_at=now()",{settings.dump(),std::to_string(who.account_id)});}
        auto result=enqueue("settings",settings);result["settings"]=settings;return result;
    }
    if(path=="/v1/admin/backups"){
        const char* configured=std::getenv("IKV_BACKUP_DIR");auto directory=std::filesystem::path(configured?configured:"backups");
        if(method=="POST"){impl_->rate("backup",2,true);auto result=backup(directory);Impl::Connection connection(*impl_);audit(connection,"database.backup",who.account_id,0,{{"name",result["name"]}});return result;}
        if(method=="GET"){Json rows=Json::array();if(std::filesystem::exists(directory))for(const auto& entry:std::filesystem::directory_iterator(directory)){auto name=entry.path().filename().string();if(!entry.is_symlink()&&entry.is_regular_file()&&name.starts_with("ikv-")&&entry.path().extension()==".sql")rows.push_back({{"name",name},{"size",entry.file_size()}});}
            std::sort(rows.begin(),rows.end(),[](const Json& a,const Json& b){return a.at("name")>b.at("name");});return {{"backups",rows}};}
    }
    throw ApiError(404,"not_found","Route not found");
}

Json Store::backup(const std::filesystem::path& directory) {
    std::filesystem::create_directories(directory);
    struct stat info{};if(lstat(directory.c_str(),&info)!=0||!S_ISDIR(info.st_mode)||info.st_uid!=geteuid())throw ApiError(500,"backup_directory","Backup directory must be owned and not a symlink");
    if(chmod(directory.c_str(),0700)!=0)throw ApiError(500,"backup_directory","Cannot restrict backup directory");
    auto name="ikv-"+std::to_string(std::chrono::system_clock::now().time_since_epoch().count())+"-"+random_hex(8)+".sql";auto file=directory/name;
    int descriptor=open(file.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);if(descriptor<0)throw ApiError(503,"backup_error","Cannot create private backup");close(descriptor);
    try {Impl::Connection connection(*impl_);process("pg_dump",{"--format=plain","--clean","--if-exists","--no-owner","--no-privileges","--file",file.string()},connection);}catch(...){std::filesystem::remove(file);throw;}
    return {{"name",name},{"size",std::filesystem::file_size(file)}};
}
void Store::restore(const std::filesystem::path& file) {
    struct stat info{};if(lstat(file.c_str(),&info)!=0||!S_ISREG(info.st_mode)||info.st_uid!=geteuid()||(info.st_mode&0077)!=0)throw ApiError(400,"restore_file","Restore requires an owned, regular 0600 backup file");
    // Every live Store (auth or world) holds shared locks; offline restore is exclusive.
    std::unique_lock guard(impl_->mutex);if(std::any_of(impl_->used.begin(),impl_->used.end(),[](bool value){return value;}))throw ApiError(409,"restore_busy","Restore must run in a separate offline process");
    for(auto* c:impl_->connections)sql(c,"SELECT pg_advisory_unlock_shared($1::bigint)",{std::string(process_lock)});
    bool exclusive=false;
    try {
        auto result=sql(impl_->connections[0],"SELECT pg_try_advisory_lock($1::bigint)",{std::string(process_lock)});
        if(!boolean(result,0,0))throw ApiError(409,"restore_busy","Stop auth and world services before restoring");
        exclusive=true;
        process("psql",{"--no-psqlrc","--set","ON_ERROR_STOP=1","--single-transaction","--file",file.string()},impl_->connections[0]);
        sql(impl_->connections[0],"SELECT pg_advisory_unlock($1::bigint)",{std::string(process_lock)});exclusive=false;
        for(auto* c:impl_->connections)sql(c,"SELECT pg_advisory_lock_shared($1::bigint)",{std::string(process_lock)});
    } catch(...) {
        if(exclusive)sql(impl_->connections[0],"SELECT pg_advisory_unlock($1::bigint)",{std::string(process_lock)});
        for(auto* c:impl_->connections)sql(c,"SELECT pg_advisory_lock_shared($1::bigint)",{std::string(process_lock)});
        throw;
    }
}
// The sentinel all-zero salt/hash never equals an Argon2id digest, so the bot account cannot log in. An account a player
// registered under the reserved name first (or whose password was reset) carries a real hash and is refused.
std::int64_t Store::bot_account() {
    Impl::Connection connection(*impl_);
    sql(connection,"INSERT INTO accounts(username,password_salt,password_hash) VALUES('ikv_bots',repeat('0',32),repeat('0',64)) ON CONFLICT(username) DO NOTHING");
    auto result=sql(connection,"SELECT id FROM accounts WHERE username='ikv_bots' AND password_hash=repeat('0',64) AND NOT admin");
    if(!PQntuples(result.get()))throw ApiError(409,"bot_account","Reserved bot account name is held by a player account");
    return integer(result,0,0);
}
Json Store::bot_characters(std::int64_t account) {
    Impl::Connection connection(*impl_);
    auto result=sql(connection,"SELECT id,name,COALESCE((state->>'level')::integer,1),COALESCE(lease_expires_at>now(),false) FROM characters WHERE account_id=$1::bigint AND deleted_at IS NULL ORDER BY id",{std::to_string(account)});
    Json rows=Json::array();
    for(int i=0;i<PQntuples(result.get());++i)rows.push_back({{"id",integer(result,i,0)},{"name",cell(result,i,1)},{"level",integer(result,i,2)},{"leased",boolean(result,i,3)}});
    return rows;
}
std::int64_t Store::create_bot_character(std::int64_t account,const std::string& name,int class_id,const Json& appearance,const Json& state) {
    if(name.size()<2||name.size()>32||class_id<1||class_id>3||!valid_appearance(appearance)||!state.is_object())throw ApiError(400,"invalid_request","Invalid bot character");
    Impl::Connection connection(*impl_);Transaction transaction(connection);
    auto result=sql(connection,"INSERT INTO characters(account_id,name,class_id,appearance,state) VALUES($1::bigint,$2,$3::integer,$4::jsonb,$5::jsonb) RETURNING id",{std::to_string(account),name,std::to_string(class_id),appearance.dump(),state.dump()});
    const auto id=integer(result,0,0);audit(connection,"character.create",account,id,{{"bot",true}});transaction.commit();return id;
}
}
