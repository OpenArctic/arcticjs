#include "Global.h"
#include "NAgent.h"

#include "AsyncWorker.h"
#include "NObject.h"

namespace arctic {

    Agent* global_agent_ = nullptr;

    Napi::Object NAgent::Init(Napi::Env env, Napi::Object exports) {
        Napi::Function constructor = DefineClass(env, "NAgent", {
            InstanceMethod<&NAgent::Start>("start"),
            InstanceMethod<&NAgent::Stop>("stop"),
            InstanceMethod<&NAgent::Export>("export"),
            InstanceMethod<&NAgent::Find>("find"),
            InstanceMethod<&NAgent::GetRoutingId>("getRoutingId"),
            });

        Napi::Object obj = constructor.New({});
        Napi::ObjectReference* ref = new Napi::ObjectReference();
        *ref = Napi::Persistent(obj);
        env.SetInstanceData(ref);
        return exports;
    }

    NAgent::NAgent(const Napi::CallbackInfo& info) : Napi::ObjectWrap<NAgent>(info) {
    }

    void NAgent::IdleTask(uv_idle_t* idle) {
        global_agent_->WorkAtIdle();
    }

    void NAgent::InstallIdleTask() {
        uv_idle_t* idle = new uv_idle_t();
        uv_idle_init(uv_default_loop(), idle);
        uv_idle_start(idle, NAgent::IdleTask);
    }

    Napi::Value NAgent::Start(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();

        int ret = agent_->Start(true);
        InstallIdleTask();
        return Napi::Number::New(env, ret);
    }

    Napi::Value NAgent::Stop(const Napi::CallbackInfo& info)
    {
        agent_->Stop();
        return Napi::Value();
    }

    Napi::Value NAgent::Export(const Napi::CallbackInfo& info)
    {
        Napi::String id_ = info[0].As<Napi::String>();
        std::string id = id_.Utf8Value();
        Napi::Object obj = info[1].As<Napi::Object>();
        if (obj.IsNull()) {
            return Napi::Value();
        }
        uint64_t raw_handle = object_factory_delegate_->RegisterObject(obj);
        Object* instance = agent_->GetObjectFactory()->Attach(object_factory_delegate_->GetType(), raw_handle);
        agent_->Export(id, instance);
        return Napi::Value();
    }

    Object* NAgent::FindInternal(uint8_t routing_id, std::string id) {
        return agent_->Find(routing_id, id).get();
    }

    Napi::Value NAgent::Find(const Napi::CallbackInfo& info)
    {
        Napi::Env env = info.Env();
        Napi::Number routing_id_ = info[0].As<Napi::Number>();
        uint8_t routing_id = (uint8_t)routing_id_.Uint32Value();
        Napi::String id_ = info[1].As<Napi::String>();
        std::string id = id_.Utf8Value();
        if (routing_id == agent_->GetRoutingId()) {
            // Do not use asyncworker when manipulating local objects, 
            // it will cause V8 objects to be manipulated in a non-main thread.
            Napi::Promise::Deferred deferred = Napi::Promise::Deferred::New(env);
            // Actually not blocked
            Object* instance = agent_->Find(routing_id, id).get();
            Napi::Object obj = NObject::NewInstance(env, instance);
            deferred.Resolve(obj);
            return deferred.Promise();
        }
        else {
            std::function<Object* ()> fn = std::bind(&NAgent::FindInternal, this, routing_id, id);
            PromiseWorker<Object*>* worker = new PromiseWorker<Object*>(env, fn);
            auto promise = worker->GetPromise();
            worker->Queue();
            return promise;
        }
    }

    Napi::Value NAgent::GetRoutingId(const Napi::CallbackInfo& info)
    {
        Napi::Env env = info.Env();

        uint8_t routing_id = agent_->GetRoutingId();
        return Napi::Number::New(env, routing_id);
    }

    Napi::Object NAgent::CreateHostAgent(const Napi::CallbackInfo& info, std::string& module_path)
    {
        HostAgentCreateParams cp;
        Napi::Number routing_id_ = info[0].As<Napi::Number>();
        cp.routing_id = (uint8_t)routing_id_.Uint32Value();
        Napi::Array child_processes_info_ = info[1].As<Napi::Array>();

        for (uint64_t i = 0; i < child_processes_info_.Length(); i++) {
            Napi::Value process_info_ = child_processes_info_[i];
            Napi::Object process_info = process_info_.As<Napi::Object>();
            uint8_t routing_id = (uint8_t)process_info.Get("routing_id").ToNumber().Uint32Value();
            std::string cmdline = process_info.Get("cmdline").ToString().Utf8Value();
            cp.child_processes_info[routing_id] = cmdline;
        }
        Napi::ObjectReference* ref = info.Env().GetInstanceData<Napi::ObjectReference>();
        NAgent* instance = NAgent::Unwrap(ref->Value());

        std::string library_path = module_path + "/lib/Arctic.dll";
        HMODULE hModule = ::LoadLibrary(library_path.c_str());
        if (hModule != NULL) {
            CreateHostAgentFunc fn = (CreateHostAgentFunc)::GetProcAddress(hModule, "CreateHostAgent");
            global_agent_ = fn(cp, nullptr);
            instance->agent_ = global_agent_;
            instance->InstallNodeJsObjectFactoryDelegate();
        }

        return ref->Value();
    }

    Napi::Object NAgent::CreateClientAgent(const Napi::CallbackInfo& info, std::string& module_path)
    {
        ClientAgentCreateParams cp;
        Napi::Number routing_id_ = info[0].As<Napi::Number>();
        cp.routing_id = (uint8_t)routing_id_.Uint32Value();
        Napi::ObjectReference* ref = info.Env().GetInstanceData<Napi::ObjectReference>();
        NAgent* instance = NAgent::Unwrap(ref->Value());

        std::string library_path = module_path + "/lib/Arctic.dll";
        HMODULE hModule = ::LoadLibrary(library_path.c_str());
        if (hModule != NULL) {
            CreateClientAgentFunc fn = (CreateClientAgentFunc)::GetProcAddress(hModule, "CreateClientAgent");
            global_agent_ = fn(cp, nullptr);
            instance->agent_ = global_agent_;
            instance->InstallNodeJsObjectFactoryDelegate();
        }

        return ref->Value();
    }

    void NAgent::InstallNodeJsObjectFactoryDelegate() {
        object_factory_delegate_ = new NodeJsObjectFactoryDelegate();
        ObjectFactory* object_factory = agent_->GetObjectFactory();
        object_factory->AddDelegate(object_factory_delegate_);
    }

    void NAgent::Finalize(Napi::Env env) {
        delete object_factory_delegate_;
        delete agent_;
    }
}