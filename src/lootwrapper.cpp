#include "lootwrapper.h"
#undef function

#include <map>
#include <future>
#include <nan.h>
#include <sstream>
#include <memory>
#include <iostream>

struct UnsupportedGame : public std::runtime_error {
  UnsupportedGame() : std::runtime_error("game not supported") {}
};

struct BusyException : public std::runtime_error {
  BusyException() : std::runtime_error("Loot connection is busy") {}
};


v8::Local<v8::String> operator "" _n(const char *input, size_t) {
  return Nan::New(input).ToLocalChecked();
}

inline v8::Local<v8::Value> CyclicalInteractionException(loot::CyclicInteractionError &err) {
  v8::Local<v8::Object> exception = Nan::Error(Nan::New<v8::String>(err.what()).ToLocalChecked()).As<v8::Object>();
  std::vector<loot::Vertex> errCycle = err.GetCycle();
  v8::Local<v8::Array> cycle = Nan::New<v8::Array>();
  int idx = 0;
  for (const auto &iter : errCycle) {
    v8::Local<v8::Object> vert = Nan::New<v8::Object>();
    std::string name = iter.GetName();
    auto n = Nan::New<v8::String>(name.c_str());
    auto n2 = n.ToLocalChecked();
    vert->Set("name"_n, n2);
    vert->Set("typeOfEdgeToNextVertex"_n, Nan::New<v8::String>(Vertex::convertEdgeType(*iter.GetTypeOfEdgeToNextVertex())).ToLocalChecked());
    cycle->Set(idx++, vert);
  }
  exception->Set("cycle"_n, cycle);

  return exception;
}

inline v8::Local<v8::Value> InvalidParameter(
  const char *func,
  const char *arg,
  const char *value) {

  std::stringstream message;
  message << "Invalid value passed to \"" << func << "\"";
  v8::Local<v8::Object> res = Nan::Error(message.str().c_str()).As<v8::Object>();
  res->Set("arg"_n, Nan::New(arg).ToLocalChecked());
  res->Set("value"_n, Nan::New(value).ToLocalChecked());
  res->Set("func"_n, Nan::New(func).ToLocalChecked());

  return res;
}

template <typename T> v8::Local<v8::Value> ToV8(const T &value) {
  return Nan::New(value);
}

template <> v8::Local<v8::Value> ToV8(const std::vector<std::string> &value) {
  v8::Local<v8::Array> res = Nan::New<v8::Array>();
  uint32_t counter = 0;
  for (const std::string &val : value) {
    res->Set(counter++, Nan::New(val.c_str()).ToLocalChecked());
  }
  return res;
}

template <typename ResT>
class Worker : public Nan::AsyncWorker {
public:
  Worker(std::function<ResT()> func, Nan::Callback *appCallback, std::function<void()> internalCallback)
    : Nan::AsyncWorker(appCallback)
    , m_Func(func)
    , m_IntCallback(internalCallback)
  {
  }

  void Execute() {
    try {
      m_Result = m_Func();
    }
    catch (const std::exception &e) {
      SetErrorMessage(e.what());
    }
    catch (...) {
      SetErrorMessage("unknown exception");
    }
  }

  void HandleOKCallback() {
    Nan::HandleScope scope;

    v8::Local<v8::Value> argv[] = {
      Nan::Null()
      , ToV8(m_Result)
    };

    m_IntCallback();
    callback->Call(2, argv);
  }

  void HandleErrorCallback() {
    m_IntCallback();
    Nan::AsyncWorker::HandleErrorCallback();
  }

private:
  ResT m_Result;
  std::function<ResT()> m_Func;
  std::function<void()> m_IntCallback;
};

template <>
class Worker<void> : public Nan::AsyncWorker {
public:
  Worker(std::function<void()> func, Nan::Callback *appCallback, std::function<void()> internalCallback)
    : Nan::AsyncWorker(appCallback)
    , m_Func(func)
    , m_IntCallback(internalCallback)
  {
  }

  void Execute() {
    try {
      m_Func();
    }
    catch (const std::exception &e) {
      SetErrorMessage(e.what());
    }
    catch (...) {
      SetErrorMessage("unknown exception");
    }
  }

  void HandleOKCallback() {
    Nan::HandleScope scope;

    v8::Local<v8::Value> argv[] = {
      Nan::Null()
    };

    m_IntCallback();
    callback->Call(1, argv);
  }

  void HandleErrorCallback() {
    m_IntCallback();
    Nan::AsyncWorker::HandleErrorCallback();
  }

private:
  std::function<void()> m_Func;
  std::function<void()> m_IntCallback;
};

Loot::Loot(std::string gameId, std::string gamePath, std::string gameLocalPath, std::string language,
           LogFunc logCallback)
  : m_Language(language)
  , m_LogCallback(logCallback)
{
  try {
    loot::InitialiseLocale(language);
    /*
    TODO: Disabled for now because it causes the process to hang when calling sortPlugins.
    loot::SetLoggingCallback([this](loot::LogLevel level, const char *message) {
      this->m_LogCallback(static_cast<int>(level), message);
    });
    */
    m_Game = loot::CreateGameHandle(convertGameId(gameId), gamePath, gameLocalPath);
  }
  catch (const std::exception &e) {
    Nan::ThrowError(e.what());
  }
  catch (...) {
    Nan::ThrowError("unknown exception");
  }
}

bool Loot::updateMasterlist(std::string masterlistPath, std::string remoteUrl, std::string remoteBranch) {
  try {
    return m_Game->GetDatabase()->UpdateMasterlist(masterlistPath, remoteUrl, remoteBranch);
  } catch (const std::exception &e) {
    NBIND_ERR(e.what());
    return false;
  }
}

void Loot::loadLists(std::string masterlistPath, std::string userlistPath)
{
  try {
    m_Game->GetDatabase()->LoadLists(masterlistPath, userlistPath);
  } catch (const std::exception &e) {
    NBIND_ERR(e.what());
  }
}

void Loot::loadPlugins(std::vector<std::string> plugins, bool loadHeadersOnly) {
  try {
    m_Game->LoadPlugins(plugins, loadHeadersOnly);
  } catch (const std::exception &e) {
    NBIND_ERR(e.what());
  }
}

PluginMetadata Loot::getPluginMetadata(std::string plugin)
{
  try {
    auto metaData = m_Game->GetDatabase()->GetPluginMetadata(plugin, true, true);
    if (!metaData.has_value()) {
      v8::Isolate* isolate = v8::Isolate::GetCurrent();
      isolate->ThrowException(InvalidParameter("getPluginMetaData", "pluginName", plugin.c_str()));
      return PluginMetadata(loot::PluginMetadata(), m_Language);
    }
    return PluginMetadata(*metaData, m_Language);
  } catch (const std::exception &e) {
    NBIND_ERR(e.what());
    return PluginMetadata(loot::PluginMetadata(), m_Language);
  }
}

PluginInterface Loot::getPlugin(const std::string &pluginName)
{
  try {
    auto plugin = m_Game->GetPlugin(pluginName);
    if (plugin.get() == nullptr) {
      NBIND_ERR("Invalid plugin name");
    }
    return PluginInterface(plugin);
  } catch (const std::exception &e) {
    NBIND_ERR(e.what());
    return PluginInterface(std::shared_ptr<loot::PluginInterface>());
  }
}

MasterlistInfo Loot::getMasterlistRevision(std::string masterlistPath, bool getShortId) const {
  try {
    return m_Game->GetDatabase()->GetMasterlistRevision(masterlistPath, getShortId);
  } catch (const std::exception &e) {
    NBIND_ERR(e.what());
    return loot::MasterlistInfo();
  }
}

std::vector<std::string> Loot::sortPlugins(std::vector<std::string> input)
{
  v8::Isolate* isolate = v8::Isolate::GetCurrent();

  try {
    return m_Game->SortPlugins(input);
  } catch (loot::CyclicInteractionError &e) {
    isolate->ThrowException(CyclicalInteractionException(e));
  } catch (const std::exception &e) {
    NBIND_ERR(e.what());
  }
  return std::vector<std::string>();
}

void Loot::setLoadOrder(std::vector<std::string> input) {
  try {
    m_Game->SetLoadOrder(input);
  } catch (const std::exception &e) {
    NBIND_ERR(e.what());
  }
}

std::vector<std::string> Loot::getLoadOrder() const {
  try {
    return m_Game->GetLoadOrder();
  } catch (const std::exception &e) {
    NBIND_ERR(e.what());
  }
  return std::vector<std::string>();
}

void Loot::loadCurrentLoadOrderState() {
  try {
    return m_Game->LoadCurrentLoadOrderState();
  } catch (const std::exception &e) {
    NBIND_ERR(e.what());
  }
}

bool Loot::isPluginActive(const std::string &pluginName) const {
  try {
    return m_Game->IsPluginActive(pluginName);
  } catch (const std::exception &e) {
    NBIND_ERR(e.what());
  }
  return false;
}

std::vector<Group> Loot::getGroups(bool includeUserMetadata) const
{
  try {
    return transform<Group>(m_Game->GetDatabase()->GetGroups(includeUserMetadata));
  } catch (const std::exception &e) {
    NBIND_ERR(e.what());
  }
  return std::vector<Group>();
}

std::vector<Group> Loot::getUserGroups() const {
  try {
    return transform<Group>(m_Game->GetDatabase()->GetUserGroups());
  } catch (const std::exception &e) {
    NBIND_ERR(e.what());
  }
  return std::vector<Group>();
}

void Loot::setUserGroups(const std::vector<Group>& groups) {
  try {
    std::unordered_set<loot::Group> result;
    for (const auto &ele : groups) {
      result.insert(ele);
    }
    m_Game->GetDatabase()->SetUserGroups(result);
  } catch (const std::exception &e) {
    NBIND_ERR(e.what());
  }
}

std::vector<Vertex> Loot::getGroupsPath(const std::string &fromGroupName, const std::string &toGroupName) const {
  try {
    return transform<Vertex>(m_Game->GetDatabase()->GetGroupsPath(fromGroupName, toGroupName));
  } catch (const std::exception &e) {
    NBIND_ERR(e.what());
  }
  return std::vector<Vertex>();
}

std::vector<Message> Loot::getGeneralMessages(bool evaluateConditions) const {
  try {
    const std::vector<loot::Message> messages = m_Game->GetDatabase()->GetGeneralMessages(evaluateConditions);
    std::vector<Message> result;
    for (const auto &msg : messages) {
      result.push_back(Message(msg, m_Language));
    }
    return result;
  } catch (const std::exception &e) {
    NBIND_ERR(e.what());
  }
  return std::vector<Message>();
}

loot::GameType Loot::convertGameId(const std::string &gameId) const {
  std::map<std::string, loot::GameType> gameMap{
    { "oblivion", loot::GameType::tes4 },
    { "skyrim", loot::GameType::tes5 },
    { "skyrimse", loot::GameType::tes5se },
    { "skyrimvr", loot::GameType::tes5vr },
    { "fallout3", loot::GameType::fo3 },
    { "falloutnv", loot::GameType::fonv },
    { "fallout4", loot::GameType::fo4 },
    { "fallout4vr", loot::GameType::fo4vr }
  };

  auto iter = gameMap.find(gameId);
  if (iter == gameMap.end()) {
    throw UnsupportedGame();
  }
  return iter->second;
}

PluginMetadata::PluginMetadata(const loot::PluginMetadata &reference, const std::string &language)
  : m_Wrapped(reference), m_Language(language)
{
}

void PluginMetadata::toJS(nbind::cbOutput output) const {
  auto group = GetGroup();
  output(GetName(), GetMessages(), GetTags(), GetCleanInfo(), GetDirtyInfo(),
    GetIncompatibilities(), GetLoadAfterFiles(), GetLocations(), GetRequirements(),
    IsEnabled(), group.has_value() ? *group : std::string());
}

inline MasterlistInfo::MasterlistInfo(loot::MasterlistInfo info)
{
  this->revision_id = info.revision_id;
  this->revision_date = info.revision_date;
  this->is_modified = info.is_modified;
}

inline void MasterlistInfo::toJS(nbind::cbOutput output) const {
  output(revision_id, revision_date, is_modified);
}

inline std::string MasterlistInfo::getRevisionId() const {
  return revision_id;
}

inline std::string MasterlistInfo::getRevisionDate() const {
  return revision_date;
}

inline bool MasterlistInfo::getIsModified() const {
  return is_modified;
}
