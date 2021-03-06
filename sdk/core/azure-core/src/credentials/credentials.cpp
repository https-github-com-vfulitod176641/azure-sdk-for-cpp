// Copyright (c) Microsoft Corporation. All rights reserved.
// SPDX-License-Identifier: MIT

#include <credentials/credentials.hpp>
#include <http/curl/curl.hpp>
#include <http/http.hpp>
#include <http/pipeline.hpp>
#include <http/stream.hpp>
#include <iomanip>
#include <sstream>
#include <stdexcept>

using namespace Azure::Core::Credentials;

namespace {
std::string UrlEncode(std::string const& s)
{
  std::ostringstream encoded;
  encoded << std::hex;

  for (auto c : s)
  {
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
        || (c == '-' || c == '.' || c == '_' || c == '~'))
    {
      encoded << c;
    }
    else
    {
      encoded << std::uppercase;
      encoded << '%' << std::setw(2) << int((unsigned char)c);
      encoded << std::nouppercase;
    }
  }

  return encoded.str();
}
} // namespace

AccessToken ClientSecretCredential::GetToken(
    Context& context,
    std::vector<std::string> const& scopes) const
{
  static std::string const errorMsgPrefix("ClientSecretCredential::GetToken: ");
  try
  {
    std::ostringstream url;
    url << "https://login.microsoftonline.com/" << UrlEncode(m_tenantId) << "/oauth2/v2.0/token";

    std::ostringstream body;
    body << "grant_type=client_credentials&client_id=" << UrlEncode(m_clientId)
         << "&client_secret=" << UrlEncode(m_clientSecret);

    if (!scopes.empty())
    {
      auto scopesIter = scopes.begin();
      body << "&scope=" << UrlEncode(*scopesIter);

      auto const scopesEnd = scopes.end();
      for (++scopesIter; scopesIter != scopesEnd; ++scopesIter)
      {
        body << " " << *scopesIter;
      }
    }

    auto const bodyString = body.str();
    std::vector<std::uint8_t> bodyVec;
    bodyVec.reserve(bodyString.size());
    for (auto c : bodyString)
    {
      bodyVec.push_back(static_cast<std::uint8_t>(c));
    }

    auto bodyStream = std::make_unique<Http::MemoryBodyStream>(bodyVec);

    Http::Request request(Http::HttpMethod::Post, url.str(), std::move(bodyStream));
    bodyStream.release();

    request.AddHeader("Content-Type", "application/x-www-form-urlencoded");
    request.AddHeader("Content-Length", std::to_string(bodyVec.size()));

    std::shared_ptr<Http::HttpTransport> transport = std::make_unique<Http::CurlTransport>();

    std::vector<std::unique_ptr<Http::HttpPolicy>> policies;
    policies.push_back(std::make_unique<Http::RequestIdPolicy>());

    Http::RetryOptions retryOptions;
    policies.push_back(std::make_unique<Http::RetryPolicy>(retryOptions));

    policies.push_back(std::make_unique<Http::TransportPolicy>(std::move(transport)));

    Http::HttpPipeline httpPipeline(policies);

    std::shared_ptr<Http::Response> response = httpPipeline.Send(context, request);

    if (!response)
    {
      throw AuthenticationException(errorMsgPrefix + "null response");
    }

    auto const statusCode = response->GetStatusCode();
    if (statusCode != Http::HttpStatusCode::Ok)
    {
      std::ostringstream errorMsg;
      errorMsg << errorMsgPrefix << "error response: "
               << static_cast<std::underlying_type<Http::HttpStatusCode>::type>(statusCode) << " "
               << response->GetReasonPhrase();

      throw AuthenticationException(errorMsg.str());
    }

    auto const responseStream = response->GetBodyStream();
    auto const responseStreamLength = responseStream->Length();

    std::string responseBody(static_cast<std::string::size_type>(responseStreamLength), 0);

    responseStream->Read(
        static_cast<std::uint8_t*>(static_cast<void*>(&responseBody[0])), responseStreamLength);

    // TODO: use JSON parser.
    auto const responseBodySize = responseBody.size();

    static std::string const jsonExpiresIn = "expires_in";
    static std::string const jsonAccessToken = "access_token";

    auto responseBodyPos = responseBody.find(':', responseBody.find(jsonExpiresIn));
    if (responseBodyPos == std::string::npos)
    {
      std::ostringstream errorMsg;
      errorMsg << errorMsgPrefix << "response json: \'" << jsonExpiresIn << "\' not found.";

      throw AuthenticationException(errorMsg.str());
    }

    for (; responseBodyPos < responseBodySize; ++responseBodyPos)
    {
      auto c = responseBody[responseBodyPos];
      if (c != ':' && c != ' ' && c != '\"' && c != '\'')
      {
        break;
      }
    }

    long long expiresInSeconds = 0;
    for (; responseBodyPos < responseBodySize; ++responseBodyPos)
    {
      auto c = responseBody[responseBodyPos];
      if (c < '0' || c > '9')
      {
        break;
      }

      expiresInSeconds = (expiresInSeconds * 10) + (c - '0');
    }

    responseBodyPos = responseBody.find(':', responseBody.find(jsonAccessToken));
    if (responseBodyPos == std::string::npos)
    {
      std::ostringstream errorMsg;
      errorMsg << errorMsgPrefix << "response json: \'" << jsonAccessToken << "\' not found.";

      throw AuthenticationException(errorMsg.str());
    }

    for (; responseBodyPos < responseBodySize; ++responseBodyPos)
    {
      auto c = responseBody[responseBodyPos];
      if (c != ':' && c != ' ' && c != '\"' && c != '\'')
      {
        break;
      }
    }

    auto const tokenBegin = responseBodyPos;
    for (; responseBodyPos < responseBodySize; ++responseBodyPos)
    {
      auto c = responseBody[responseBodyPos];
      if (c == '\"' || c == '\'')
      {
        break;
      }
    }
    auto const tokenEnd = responseBodyPos;

    expiresInSeconds -= 2 * 60;
    auto const responseBodyBegin = responseBody.begin();

    return {
        std::string(responseBodyBegin + tokenBegin, responseBodyBegin + tokenEnd),
        std::chrono::system_clock::now()
            + std::chrono::seconds(expiresInSeconds < 0 ? 0 : expiresInSeconds),
    };
  }
  catch (AuthenticationException const&)
  {
    throw;
  }
  catch (std::exception const& e)
  {
    throw new AuthenticationException(e.what());
  }
  catch (...)
  {
    throw new AuthenticationException("unknown error");
  }
}
