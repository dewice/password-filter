#include "pch.h"
#include "idmRestComm.h"
#include "configuration.h"
#include "logger.h"

#include <winhttp.h>


/****Global objects****/
extern Logger gLogger;
extern Configuration gConfiguration;

/**
* checkIdmPolicies method queries IdM whether the password supplied in the request body meets password policies  
* returns TRUE if password is supposed to be changed on AD otherwise FALSE is returned
*/
bool IdmRestComm::checkIdmPolicies(const IdmRequestCont& body)
{
   gLogger.log(Logger::DEBUG(), "Calling checkIdmPolicies");
   bool securityFailure = false;
   const auto& urlBaseVec = gConfiguration.getRestBaseUrlVec();
   // iterate over alternative base urls if connection fails
   for (const ut::string_t& baseUrl : urlBaseVec) 
   {  // try as many attempts as set in case of failure
      securityFailure = false;
      for (uint32_t attemptCnt = gConfiguration.getConnectionAttempts(); attemptCnt > 0; --attemptCnt)
      {
         try
         {
            wh::uri_builder urlBuild(baseUrl);
            urlBuild.append(gConfiguration.getRestCheckUrl());
            wh::uri destUrl = urlBuild.to_uri();
            auto requestTask = createRequestTask(wh::methods::PUT, destUrl, body.toJsonObject());
            wh::http_response response = requestTask.get();
            IdmResponseCont responseCont(response);
            IdmResponseCont::passFiltAction action = responseCont.getPassFiltAction();

            switch (action)
            {
            case IdmResponseCont::PF_ACT_TRUE:
               return true;
               //break;
            case IdmResponseCont::PF_ACT_FALSE:
               return false;
               //break;
            case IdmResponseCont::PF_ACT_CFG_DEFAULT:
               return gConfiguration.getAllowChangeByDefault();
               //break;
            case IdmResponseCont::PF_ACT_TRY_AGAIN:
                  continue;
               //break;
            default:
               continue;
               //break;
            }
         }
         catch (const wj::json_exception& jsonEx)
         {
            gLogger.log(Logger::ERROR(), "An error occurred when parsing validation response: %s", jsonEx.what());
         }
         catch (const wh::http_exception& httpEx)
         {
            gLogger.log(Logger::ERROR(), "A WinHttp exception occurred: %s ", httpEx.what());
            securityFailure = isSecurityFailure(httpEx);
         }
         catch (const std::exception& ex)
         {
            gLogger.log(Logger::ERROR(), "An unexpected error occurred in checkIdmPolicies: %s", ex.what());
         }
      }
   }
   if (securityFailure) // return false in case of secure connection troubles
      return false;
   return gConfiguration.getAllowChangeByDefault(); // this value is returned as the failback if something unpredictable happend
}


/**
* notifyIdm method informs IdM that password met all policies and has been changed on AD
*
*/
void IdmRestComm::notifyIdm(const IdmRequestCont& body)
{
   gLogger.log(Logger::DEBUG(), "Calling notifyIdm");
   const auto& urlBaseVec = gConfiguration.getRestBaseUrlVec();
   // iterate over alternative base urls if connection fails
   for (const ut::string_t& baseUrl : urlBaseVec) 
   {  // try as many attempts as set in case of failure
      for (uint32_t attemptCnt = gConfiguration.getConnectionAttempts(); attemptCnt > 0; --attemptCnt)
      {
         try
         {
            wh::uri_builder urlBuild(baseUrl);
            urlBuild.append(gConfiguration.getRestNotifyUrl());
            wh::uri destUrl = urlBuild.to_uri();
            auto requestTask = createRequestTask(wh::methods::PUT, destUrl, body.toJsonObject());
            wh::http_response response = requestTask.get();
            auto httpStatus = response.status_code();
            if (httpStatus == wh::status_codes::OK)
            {
               gLogger.log(Logger::INFO(), "Idm successfully notified");
               return;
            }
            else
            {
               gLogger.log(Logger::WARN(), "Idm notify function received http status: %u", httpStatus);
               return;
            }
         }
         catch (const wh::http_exception& httpEx)
         {
            gLogger.log(Logger::ERROR(), "A WinHttp exception occurred: %s ", httpEx.what());
         }
         catch (const std::exception& ex)
         {
            gLogger.log(Logger::ERROR(), "An unexpected error occurred in notifyIdm: %s", ex.what());
         }
      }
   }
}

/**
* createRequestTask method encapsulates creating of configured REST request. 
* The request is run as a task in separate thread.
*/
cnc::task<wh::http_response> IdmRestComm::createRequestTask(const wh::method& method, const wh::uri& url, const wj::value& body)
{
   wh::http_request request(method);
   wh::http_headers& head = request.headers();
   addTokenAuthentication(head);
   head.set_content_type(sIdmContentType);

   // set request boody
   if (method != wh::methods::GET)
   {
      request.set_body(body);
   }
   
   // client config options
   wh::client::http_client_config clientConfig;
   clientConfig.set_timeout(std::chrono::milliseconds(gConfiguration.getConnectionTimeoutMs()));
   clientConfig.set_validate_certificates(!gConfiguration.getIgnoreCertificate());

   wh::client::http_client client(url, clientConfig);
   return client.request(request);
}

void IdmRestComm::addTokenAuthentication(web::http::http_headers& head) const
{
   const ut::string_t& token = gConfiguration.getToken();
   head.add(U("CIDMST"), token);
}

bool IdmRestComm::isSecurityFailure(const wh::http_exception& e)
{
   const static char* sslErrors[] = {
      "WINHTTP_CALLBACK_STATUS_FLAG_CERT_REV_FAILED",
      "WINHTTP_CALLBACK_STATUS_FLAG_INVALID_CERT",
      "WINHTTP_CALLBACK_STATUS_FLAG_CERT_REVOKED",
      "WINHTTP_CALLBACK_STATUS_FLAG_INVALID_CA",
      "WINHTTP_CALLBACK_STATUS_FLAG_CERT_CN_INVALID",
      "WINHTTP_CALLBACK_STATUS_FLAG_CERT_DATE_INVALID",
      "WINHTTP_CALLBACK_STATUS_FLAG_SECURITY_CHANNEL_ERROR"
   };

   std::string ex(e.what());
   for (const char *err : sslErrors)
   {
      std::string sub(err);
      if (ex.find(sub) != std::string::npos)
         return true;
   }
   return false;
}

///////////////// IdmRequestCont //////////////////////////////

IdmRequestCont::~IdmRequestCont()
{
   auto ptr = mPassword.data();
   auto size = mPassword.size();
   SecureZeroMemory(ptr, size*sizeof(ptr[0]));
}

ut::string_t IdmRequestCont::toJsonString16() const
{
   ut::string_t out = toJsonObject().serialize();
   return out;
}

std::string IdmRequestCont::toJsonString8() const
{
   return uc::to_utf8string(toJsonString16());
}

wj::value IdmRequestCont::toJsonObject() const
{
   wj::value obj;
   obj[mAccountKey] = wj::value::string(mAccountName);
   obj[mPasswordKey] = wj::value::string(mPassword);
   obj[mSystemKey] = wj::value::string(mSystemName);
   obj[mLogIdKey] = wj::value::string(mLogId);
   obj[mVersionKey] = wj::value::string(gConfiguration.getVersion());
   return obj;
}

bool IdmRequestCont::accountStartsWithReservedChar()
{
   const ut::string_t& reserved = gConfiguration.getForbiddenInitChars();
   if (mAccountName.size() == 0 || reserved.size() == 0)
      return false;

   for (ut::string_t::value_type ch : reserved)
   {
      if (ch == mAccountName[0])
         return true;
   }
   return false;
}

ut::string_t IdmRequestCont::pUnicode2String(const PUNICODE_STRING uniStr) const
{
   if (uniStr != nullptr && uniStr->Buffer != nullptr && uniStr->Length > 0 )
      return ut::string_t(uniStr->Buffer, uniStr->Length / sizeof(uniStr->Buffer[0]));
   else
      return ut::string_t();
}


///////////// IdmResponseCont /////////////////

IdmResponseCont::IdmResponseCont(const wh::http_response& response)
{
   try
   {
      const wh::http_headers& head = response.headers();
      ut::string_t contentType = head.content_type();
      mResultCode = response.status_code();
      const ut::string_t jsonStr = response.extract_utf16string(true).get();
      mHasIdmContent = parseJson(jsonStr);
   }
   catch (const std::exception& e)
   {
      gLogger.log(Logger::WARN(), "An exception occurred during parsing of the validation response: %s", e.what());
   }
   mPassFiltAction = deducePassFiltAction();
}

bool IdmResponseCont::parseJson(const ut::string_t& jsonStr)
{
   try
   {
      wj::value rootObj = wj::value::parse(jsonStr);
      if (!Configuration::proveKeyPresence(rootObj, sErrorKey, &wj::value::has_array_field))
         return false;
      const auto& errors = rootObj.at(sErrorKey).as_array();
      if (errors.size() == 0)
         return false;

      const wj::value& error = errors.at(0);
      if (!Configuration::proveKeyPresence(error, sStatusEnumKey, &wj::value::has_string_field))
         return false;
      mStatusEnum = error.at(sStatusEnumKey).as_string();
      return true;
   }
   catch (const wj::json_exception&)
   {
      return false;
   }
}

IdmResponseCont::passFiltAction IdmResponseCont::deducePassFiltAction() const
{
   // pass validation is OK
   if (mResultCode == wh::status_codes::OK)
   {
      gLogger.log(Logger::INFO(), "Password policy validation passed");
      return passFiltAction::PF_ACT_TRUE;
   }

   // pass filter is disabled and therefore bypassed
   if (mResultCode == wh::status_codes::Locked)
   {
      gLogger.log(Logger::INFO(), "Password filter is disabled in Idm");
      return passFiltAction::PF_ACT_TRUE;
   }
      
   // http result code 400
   if (mResultCode == wh::status_codes::BadRequest)
   {
      if (mHasIdmContent && mStatusEnum.compare(sPolicyValidationFailed) == 0)
      {
         gLogger.log(Logger::INFO(), "Password does not meet password policies");
         return passFiltAction::PF_ACT_FALSE; //pass policy validation failed
      }
   }

   // http result code 404
   if (mResultCode == wh::status_codes::NotFound)
   {
      if (mHasIdmContent && (
         mStatusEnum.compare(sSystemNotFound) == 0 ||
         mStatusEnum.compare(sIdentityNotFound) == 0 ||
         mStatusEnum.compare(sDefinitionNotFound) == 0))
      {
         gLogger.log(Logger::INFO(), "Some of searched entities are missing in Idm: %s", Logger::w2s(mStatusEnum).c_str());
         return passFiltAction::PF_ACT_TRUE; // any Idm item (identity, system, account) wasn't found; PF has to allow pass change
      }
   }

   if (mResultCode == wh::status_codes::RequestTimeout ||
      mResultCode == wh::status_codes::GatewayTimeout)
   {
      gLogger.log(Logger::INFO(), "Some kind of timeout response occurred. Http status: %u", mResultCode);
      return PF_ACT_TRY_AGAIN;
   }

   gLogger.log(Logger::INFO(), "Password filter received a response with http status: %u and Idm statusEnum: %s", mResultCode, Logger::w2s(mStatusEnum).c_str());
   return PF_ACT_FALSE;
}
