#include "ScriptEngine.hpp"

#ifdef SCRIPT_ENGINE_JSC

#include "Object.hpp"
#include "Class.hpp"
#include "Utils.hpp"
#include "../MappingUtils.hpp"

extern "C" JS_EXPORT void JSSynchronousGarbageCollectForDebugging(JSContextRef);

namespace se {

    AutoHandleScope::AutoHandleScope()
    {

    }

    AutoHandleScope::~AutoHandleScope()
    {

    }

    Class* __jsb_CCPrivateData_class = nullptr;
    //
    namespace {

        ScriptEngine* __instance = nullptr;

        JSValueRef __forceGC(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject,
                             size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception)
        {
            LOGD("GC begin ..., (Native -> JS map) count: %d\n", (int)NativePtrToObjectMap::size());
//            JSGarbageCollect(ctx);
            JSSynchronousGarbageCollectForDebugging(ctx);
            LOGD("GC end ..., (Native -> JS map) count: %d\n", (int)NativePtrToObjectMap::size());
            return JSValueMakeUndefined(ctx);
        }

        JSValueRef __log(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject,
                         size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception)
        {
            if (argumentCount > 0)
            {
                std::string ret;
                internal::forceConvertJsValueToStdString(ctx, arguments[0], &ret);
                LOGD("%s\n", ret.c_str());
            }
            return JSValueMakeUndefined(ctx);
        }

        JSObjectRef privateDataContructor(JSContextRef ctx, JSObjectRef constructor, size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception)
        {
            return nullptr;
        }

        void privateDataFinalize(JSObjectRef obj)
        {
            internal::PrivateData* p = (internal::PrivateData*)JSObjectGetPrivate(obj);
            JSObjectSetPrivate(obj, p->data);
            if (p->finalizeCb != nullptr)
                p->finalizeCb(obj);
            free(p);
        }
    }

    ScriptEngine *ScriptEngine::getInstance()
    {
        if (__instance == nullptr)
        {
            __instance = new ScriptEngine();
        }

        return __instance;
    }

    void ScriptEngine::destroyInstance()
    {
        delete __instance;
        __instance = nullptr;
    }

    ScriptEngine::ScriptEngine()
            : _cx(nullptr)
            , _globalObj(nullptr)
            , _isGarbageCollecting(false)
            , _isValid(false)
            , _isInCleanup(false)
            , _isErrorHandleWorking(false)
            , _exceptionCallback(nullptr)
    {
    }

    bool ScriptEngine::init()
    {
        cleanup();
        LOGD("Initializing JavaScriptCore \n");

        for (const auto& hook : _beforeInitHookArray)
        {
            hook();
        }
        _beforeInitHookArray.clear();

        _cx = JSGlobalContextCreate(nullptr);

        if (nullptr == _cx)
            return false;

        JSStringRef ctxName = JSStringCreateWithUTF8CString("Cocos2d-x JSB");
        JSGlobalContextSetName(_cx, ctxName);
        JSStringRelease(ctxName);

        NativePtrToObjectMap::init();
        NonRefNativePtrCreatedByCtorMap::init();
        
        internal::setContext(_cx);
        Class::setContext(_cx);
        Object::setContext(_cx);

        JSObjectRef globalObj = JSContextGetGlobalObject(_cx);

        if (nullptr == globalObj)
            return false;

        _globalObj = Object::_createJSObject(nullptr, globalObj);
        _globalObj->root();
        _globalObj->setProperty("window", se::Value(_globalObj));

        _globalObj->setProperty("scriptEngineType", se::Value("JavaScriptCore"));

        JSStringRef propertyName = JSStringCreateWithUTF8CString("log");
        JSObjectSetProperty(_cx, globalObj, propertyName, JSObjectMakeFunctionWithCallback(_cx, propertyName, __log), kJSPropertyAttributeReadOnly, nullptr);
        JSStringRelease(propertyName);

        propertyName = JSStringCreateWithUTF8CString("forceGC");
        JSObjectSetProperty(_cx, globalObj, propertyName, JSObjectMakeFunctionWithCallback(_cx, propertyName, __forceGC), kJSPropertyAttributeReadOnly, nullptr);
        JSStringRelease(propertyName);

        __jsb_CCPrivateData_class = Class::create("__CCPrivateData", _globalObj, nullptr, privateDataContructor);
        __jsb_CCPrivateData_class->defineFinalizeFunction(privateDataFinalize);
        __jsb_CCPrivateData_class->install();

        _isValid = true;

        for (const auto& hook : _afterInitHookArray)
        {
            hook();
        }
        _afterInitHookArray.clear();

        return true;
    }

    ScriptEngine::~ScriptEngine()
    {
        cleanup();
    }

    void ScriptEngine::cleanup()
    {
        if (!_isValid)
            return;

        LOGD("ScriptEngine::cleanup begin ...\n");
        _isInCleanup = true;
        for (const auto& hook : _beforeCleanupHookArray)
        {
            hook();
        }
        _beforeCleanupHookArray.clear();

        SAFE_DEC_REF(_globalObj);
        Object::cleanup();
        Class::cleanup();
        garbageCollect();

        JSGlobalContextRelease(_cx);

        _cx = nullptr;
        _globalObj = nullptr;
        _isValid = false;
        _nodeEventListener = nullptr;

        _registerCallbackArray.clear();

        for (const auto& hook : _afterCleanupHookArray)
        {
            hook();
        }
        _afterCleanupHookArray.clear();
        _isInCleanup = false;

        NativePtrToObjectMap::destroy();
        NonRefNativePtrCreatedByCtorMap::destroy();
        LOGD("ScriptEngine::cleanup end ...\n");
    }

    ScriptEngine::ExceptionInfo ScriptEngine::_formatException(JSValueRef exception)
    {
        ExceptionInfo ret;
        internal::forceConvertJsValueToStdString(_cx, exception, &ret.message);

        JSType type = JSValueGetType(_cx, exception);

        if (type == kJSTypeObject)
        {
            JSObjectRef obj = JSValueToObject(_cx, exception, nullptr);
            JSStringRef stackKey = JSStringCreateWithUTF8CString("stack");
            JSValueRef stackVal = JSObjectGetProperty(_cx, obj, stackKey, nullptr);
            if (stackKey != nullptr)
            {
                type = JSValueGetType(_cx, stackVal);
                if (type == kJSTypeString)
                {
                    JSStringRef stackStr = JSValueToStringCopy(_cx, stackVal, nullptr);
                    internal::jsStringToStdString(_cx, stackStr, &ret.stack);
                    JSStringRelease(stackStr);
                }
                JSStringRelease(stackKey);
            }

            std::string line;
            std::string column;
            std::string filePath;
            JSPropertyNameArrayRef nameArr = JSObjectCopyPropertyNames(_cx, obj);
            size_t count =JSPropertyNameArrayGetCount(nameArr);
            for (size_t i = 0; i < count; ++i)
            {
                JSStringRef jsName = JSPropertyNameArrayGetNameAtIndex(nameArr, i);
                JSValueRef jsValue = JSObjectGetProperty(_cx, obj, jsName, nullptr);

                std::string name;
                internal::jsStringToStdString(_cx, jsName, &name);
                std::string value;

                JSStringRef jsstr = JSValueToStringCopy(_cx, jsValue, nullptr);
                internal::jsStringToStdString(_cx, jsstr, &value);
                JSStringRelease(jsstr);

                if (name == "line")
                {
                    line = value;
                    ret.lineno = (uint32_t)JSValueToNumber(_cx, jsValue, nullptr);
                }
                else if (name == "column")
                {
                    column = value;
                }
                else if (name == "sourceURL")
                {
                    filePath = value;
                    ret.filePath = value;
                }
            }

            ret.location = filePath + ":" + line + ":" + column;
            JSPropertyNameArrayRelease(nameArr);
        }

        return ret;
    }

    void ScriptEngine::_clearException(JSValueRef exception)
    {
        if (exception != nullptr)
        {
            ExceptionInfo exceptionInfo = _formatException(exception);
            if (exceptionInfo.isValid())
            {
                std::string exceptionStr = exceptionInfo.message;
                exceptionStr += ", location: " + exceptionInfo.location;
                if (!exceptionInfo.stack.empty())
                {
                    exceptionStr += "\nSTACK:\n" + exceptionInfo.stack;
                }
                LOGD("ERROR: %s\n", exceptionStr.c_str());

                if (_exceptionCallback != nullptr)
                {
                    _exceptionCallback(exceptionInfo.location.c_str(), exceptionInfo.message.c_str(), exceptionInfo.stack.c_str());
                }

                if (!_isErrorHandleWorking)
                {
                    _isErrorHandleWorking = true;

                    Value errorHandler;
                    if (_globalObj->getProperty("__errorHandler", &errorHandler) && errorHandler.isObject() && errorHandler.toObject()->isFunction())
                    {
                        ValueArray args;
                        args.push_back(Value(exceptionInfo.filePath));
                        args.push_back(Value(exceptionInfo.lineno));
                        args.push_back(Value(exceptionInfo.message));
                        args.push_back(Value(exceptionInfo.stack));
                        errorHandler.toObject()->call(args, _globalObj);
                    }

                    _isErrorHandleWorking = false;
                }
                else
                {
                    LOGE("ERROR: __errorHandler has exception\n");
                }
            }
        }
    }

    void ScriptEngine::setExceptionCallback(const ExceptionCallback& cb)
    {
        _exceptionCallback = cb;
    }

    bool ScriptEngine::isGarbageCollecting()
    {
        return _isGarbageCollecting;
    }

    void ScriptEngine::_setGarbageCollecting(bool isGarbageCollecting)
    {
        _isGarbageCollecting = isGarbageCollecting;
    }

    Object* ScriptEngine::getGlobalObject()
    {
        return _globalObj;
    }

    void ScriptEngine::addBeforeInitHook(const std::function<void()>& hook)
    {
        _beforeInitHookArray.push_back(hook);
    }

    void ScriptEngine::addAfterInitHook(const std::function<void()>& hook)
    {
        _afterInitHookArray.push_back(hook);
    }

    void ScriptEngine::addBeforeCleanupHook(const std::function<void()>& hook)
    {
        _beforeCleanupHookArray.push_back(hook);
    }

    void ScriptEngine::addAfterCleanupHook(const std::function<void()>& hook)
    {
        _afterCleanupHookArray.push_back(hook);
    }

    void ScriptEngine::addRegisterCallback(RegisterCallback cb)
    {
        assert(std::find(_registerCallbackArray.begin(), _registerCallbackArray.end(), cb) == _registerCallbackArray.end());
        _registerCallbackArray.push_back(cb);
    }

    bool ScriptEngine::start()
    {
        if (!init())
            return false;

        bool ok = false;
        _startTime = std::chrono::steady_clock::now();

        for (auto cb : _registerCallbackArray)
        {
            ok = cb(_globalObj);
            assert(ok);
            if (!ok)
                break;
        }

        // After ScriptEngine is started, _registerCallbackArray isn't needed. Therefore, clear it here.
        _registerCallbackArray.clear();
        return ok;
    }

    void ScriptEngine::garbageCollect()
    {
        LOGD("GC begin ..., (Native -> JS map) count: %d\n", (int)NativePtrToObjectMap::size());
        // JSGarbageCollect(_cx);
        JSSynchronousGarbageCollectForDebugging(_cx);
        LOGD("GC end ..., (Native -> JS map) count: %d\n", (int)NativePtrToObjectMap::size());
    }

    bool ScriptEngine::evalString(const char* script, ssize_t length/* = -1 */, Value* ret/* = nullptr */, const char* fileName/* = nullptr */)
    {
        assert(script != nullptr);
        if (length < 0)
            length = strlen(script);

        if (fileName == nullptr)
            fileName = "(no filename)";

        std::string exceptionStr;
        std::string scriptStr(script, length);

        JSValueRef exception = nullptr;
        JSStringRef jsSourceUrl = JSStringCreateWithUTF8CString(fileName);
        JSStringRef jsScript = JSStringCreateWithUTF8CString(scriptStr.c_str());
        JSValueRef result = nullptr;

        bool ok = JSCheckScriptSyntax(_cx, jsScript, jsSourceUrl, 1, &exception);;
        if (ok)
        {
            result = JSEvaluateScript(_cx, jsScript, nullptr, jsSourceUrl, 1, &exception);

            if (exception != nullptr)
            {
                ok = false;
            }
        }
        else
        {
            if (exception == nullptr)
            {
                LOGD("Unknown syntax error parsing file %s\n", fileName);
            }
        }

        JSStringRelease(jsScript);
        JSStringRelease(jsSourceUrl);

        if (ok)
        {
            if (ret != nullptr)
                internal::jsToSeValue(_cx, result, ret);
        }

        _clearException(exception);

        return ok;
    }

    void ScriptEngine::setFileOperationDelegate(const FileOperationDelegate& delegate)
    {
        _fileOperationDelegate = delegate;
    }

    bool ScriptEngine::runScript(const std::string& path, Value* ret/* = nullptr */)
    {
        assert(!path.empty());
        assert(_fileOperationDelegate.isValid());

        std::string scriptBuffer = _fileOperationDelegate.onGetStringFromFile(path);

        if (!scriptBuffer.empty())
        {
            return evalString(scriptBuffer.c_str(), scriptBuffer.length(), ret, path.c_str());
        }

        LOGE("ScriptEngine::runScript script %s, buffer is empty!\n", path.c_str());
        return false;
    }

    void ScriptEngine::_retainScriptObject(void* owner, void* target)
    {
        auto iterOwner = NativePtrToObjectMap::find(owner);
        if (iterOwner == NativePtrToObjectMap::end())
        {
            return;
        }

        auto iterTarget = NativePtrToObjectMap::find(target);
        if (iterTarget == NativePtrToObjectMap::end())
        {
            return;
        }

        clearException();
        iterOwner->second->attachObject(iterTarget->second);
    }

    void ScriptEngine::_releaseScriptObject(void* owner, void* target)
    {
        auto iterOwner = NativePtrToObjectMap::find(owner);
        if (iterOwner == NativePtrToObjectMap::end())
        {
            return;
        }

        auto iterTarget = NativePtrToObjectMap::find(target);
        if (iterTarget == NativePtrToObjectMap::end())
        {
            return;
        }

        clearException();
        iterOwner->second->detachObject(iterTarget->second);
    }

    bool ScriptEngine::_onReceiveNodeEvent(void* node, NodeEventType type)
    {
        assert(_nodeEventListener != nullptr);
        return _nodeEventListener(node, type);
    }

    bool ScriptEngine::_setNodeEventListener(NodeEventListener listener)
    {
        _nodeEventListener = listener;
        return true;
    }

    void ScriptEngine::clearException()
    {
        //FIXME:
    }

    void ScriptEngine::enableDebugger(unsigned int port/* = 5086*/)
    {
        // empty implementation
    }

    void ScriptEngine::mainLoopUpdate()
    {
        // empty implementation
    }

} // namespace se {

#endif // SCRIPT_ENGINE_JSC
