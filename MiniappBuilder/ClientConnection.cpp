#include "ClientConnection.h"

#include <limits.h>
#include <stddef.h>

#include <WinSock2.h>
#include <filesystem>

#include "DeviceManager.hpp"
#include "AnisetteDataManager.h"
#include "AnisetteData.h"
#include "MiniappBuilderCore.h"

#include "ServerError.hpp"

#include <codecvt>

#define odslog(msg) {  std::cout << msg << std::endl; }

extern std::string make_uuid();
extern std::string temporary_directory();

using namespace web;

namespace fs = std::filesystem;

std::string StringFromWideString(std::wstring wideString)
{
	std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;

	std::string string = converter.to_bytes(wideString);
	return string;
}

std::wstring WideStringFromString(std::string string)
{
	std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;

	std::wstring wideString = converter.from_bytes(string);
	return wideString;
}

ClientConnection::ClientConnection()
{
}

ClientConnection::~ClientConnection()
{
	this->Disconnect();
}

void ClientConnection::Disconnect()
{
}

pplx::task<void> ClientConnection::ProcessAppRequest()
{
	auto task = this->ReceiveRequest().then([this](web::json::value request) {
		auto identifier = StringFromWideString(request[L"identifier"].as_string());

		if (identifier == "PrepareAppRequest")
		{
			return this->ProcessPrepareAppRequest(request);
		}
		else if (identifier == "AnisetteDataRequest")
		{
			return this->ProcessAnisetteDataRequest(request);
		}
		else if (identifier == "InstallProvisioningProfilesRequest")
		{
			return this->ProcessInstallProfilesRequest(request);
		}
		else if (identifier == "RemoveProvisioningProfilesRequest")
		{
			return this->ProcessRemoveProfilesRequest(request);
		}
		else if (identifier == "RemoveAppRequest")
		{
			return this->ProcessRemoveAppRequest(request);
		}
        else if (identifier == "EnableUnsignedCodeExecutionRequest")
        {
            return this->ProcessEnableUnsignedCodeExecutionRequest(request);
        }
		else
		{
			throw ServerError(ServerErrorCode::UnknownRequest);
		}
	}).then([this](pplx::task<void> task) {
		try
		{
			task.get();
		}
		catch (std::exception& e)
		{
			auto errorResponse = this->ErrorResponse(e);
			this->SendResponse(errorResponse).then([=](pplx::task<void> task) {
				try
				{
					task.get();
				}
				catch (Error& error)
				{
					odslog("[ALTLog] Failed to send error response: " << error.localizedDescription());
				}
				catch (std::exception& exception)
				{
					odslog("[ALTLog] Failed to send error response: " << exception.what());
				}
			});

			throw;
		}		
	});

	return task;
}

pplx::task<void> ClientConnection::ProcessPrepareAppRequest(web::json::value request)
{
	utility::string_t* filepath = new utility::string_t;
	std::string udid = StringFromWideString(request[L"udid"].as_string());

	return this->ReceiveApp(request).then([this, filepath](std::string path) {
		*filepath = WideStringFromString(path);
		return this->ReceiveRequest();
	})
	.then([this, filepath, udid](web::json::value request) {
		std::optional<std::set<std::string>> activeProfiles = std::nullopt;

		if (request.has_array_field(L"activeProfiles"))
		{
			activeProfiles = std::set<std::string>();

			auto array = request[L"activeProfiles"].as_array();
			for (auto& value : array)
			{
				auto bundleIdentifier = value.as_string();
				activeProfiles->insert(StringFromWideString(bundleIdentifier));
			}
		}

		return this->InstallApp(StringFromWideString(*filepath), udid, activeProfiles);
	})
	.then([this, filepath, udid](pplx::task<void> task) {

		if (filepath->size() > 0)
		{
			try
			{
				fs::remove(fs::path(*filepath));
			}
			catch (std::exception& e)
			{
				odslog("Failed to remove received .ipa." << e.what());
			}
		}

		delete filepath;		

		try
		{
			task.get();

			auto response = json::value::object();
			response[L"version"] = json::value::number(1);
			response[L"identifier"] = json::value::string(L"InstallationProgressResponse");
			response[L"progress"] = json::value::number(1.0);
			return this->SendResponse(response);
		}
		catch (std::exception& exception)
		{
			throw;
		}
	});
}

pplx::task<void> ClientConnection::ProcessAnisetteDataRequest(web::json::value request)
{
	return pplx::create_task([this, &request]() {

		auto anisetteData = AnisetteDataManager::instance()->FetchAnisetteData();
		if (!anisetteData)
		{
			throw ServerError(ServerErrorCode::InvalidAnisetteData);
		}
			
		auto response = json::value::object();
		response[L"version"] = json::value::number(1);
		response[L"identifier"] = json::value::string(L"AnisetteDataResponse");
		response[L"anisetteData"] = anisetteData->json();
		return this->SendResponse(response);
	});
}

pplx::task<std::string> ClientConnection::ReceiveApp(web::json::value request)
{
	auto appSize = request[L"contentSize"].as_integer();
	std::cout << "Receiving app (" << appSize << " bytes)..." << std::endl;

	return this->ReceiveData(appSize).then([this](std::vector<unsigned char> data) {
		fs::path filepath = fs::path(temporary_directory()).append(make_uuid() + ".ipa");

		std::ofstream file(filepath.string(), std::ios::out | std::ios::binary);
		copy(data.cbegin(), data.cend(), std::ostreambuf_iterator<char>(file));

		return filepath.string();
	});
}

pplx::task<void> ClientConnection::InstallApp(std::string filepath, std::string udid, std::optional<std::set<std::string>> activeProfiles)
{
	return pplx::create_task([this, filepath, udid, activeProfiles]() {
		try {
			auto isSending = std::make_shared<bool>();

			return DeviceManager::instance()->InstallApp(filepath, udid, activeProfiles, [this, isSending](double progress) {
				if (*isSending)
				{
					return;
				}

				*isSending = true;

				auto response = json::value::object();
				response[L"version"] = json::value::number(1);
				response[L"identifier"] = json::value::string(L"InstallationProgressResponse");
				response[L"progress"] = json::value::number(progress);

				this->SendResponse(response).then([isSending](pplx::task<void> task) {
					try
					{
						task.get();

						// Only set to false if there wasn't an error sending progress.
						*isSending = false;
					}
					catch (Error& error)
					{
						odslog("[ALTLog] Error sending installation progress: " << error.localizedDescription());
					}
					catch (std::exception &exception)
					{
						odslog("[ALTLog] Error sending installation progress: " << exception.what());
					}					
				});
			});
		}
		catch (Error& error)
		{
			std::cout << error << std::endl;

			throw error;
		}
		catch (std::exception& e)
		{
			std::cout << "Exception: " << e.what() << std::endl;

			throw e;
		}
		std::cout << "Installed app!" << std::endl;
	});
}

pplx::task<void> ClientConnection::ProcessInstallProfilesRequest(web::json::value request)
{
	std::string udid = StringFromWideString(request[L"udid"].as_string());

	std::vector<std::shared_ptr<ProvisioningProfile>> provisioningProfiles;

	auto array = request[L"provisioningProfiles"].as_array();
	for (auto& value : array)
	{
		auto encodedData = value.as_string();
		auto data = utility::conversions::from_base64(encodedData);

		auto profile = std::make_shared<ProvisioningProfile>(data);
		if (profile != nullptr)
		{
			provisioningProfiles.push_back(profile);
		}
	}

	std::optional<std::set<std::string>> activeProfiles = std::nullopt;
	if (request.has_array_field(L"activeProfiles"))
	{
		activeProfiles = std::set<std::string>();

		auto array = request[L"activeProfiles"].as_array();
		for (auto& value : array)
		{
			auto bundleIdentifier = value.as_string();
			activeProfiles->insert(StringFromWideString(bundleIdentifier));
		}
	}

	return DeviceManager::instance()->InstallProvisioningProfiles(provisioningProfiles, udid, activeProfiles)
	.then([=](pplx::task<void> task) {
		try
		{
			task.get();

			auto response = json::value::object();
			response[L"version"] = json::value::number(1);
			response[L"identifier"] = json::value::string(L"InstallProvisioningProfilesResponse");
			return this->SendResponse(response);
		}
		catch (std::exception& exception)
		{
			throw;
		}
	});
}

pplx::task<void> ClientConnection::ProcessRemoveProfilesRequest(web::json::value request)
{
	std::string udid = StringFromWideString(request[L"udid"].as_string());

	std::set<std::string> bundleIdentifiers;

	auto array = request[L"bundleIdentifiers"].as_array();
	for (auto& value : array)
	{
		auto bundleIdentifier = StringFromWideString(value.as_string());
		bundleIdentifiers.insert(bundleIdentifier);
	}

	return DeviceManager::instance()->RemoveProvisioningProfiles(bundleIdentifiers, udid)
	.then([=](pplx::task<void> task) {
		try
		{
			task.get();

			auto response = json::value::object();
			response[L"version"] = json::value::number(1);
			response[L"identifier"] = json::value::string(L"RemoveProvisioningProfilesResponse");
			return this->SendResponse(response);
		}
		catch (std::exception& exception)
		{
			throw;
		}
	});
}

pplx::task<void> ClientConnection::ProcessRemoveAppRequest(web::json::value request)
{
	std::string udid = StringFromWideString(request[L"udid"].as_string());
	auto bundleIdentifier = StringFromWideString(request[L"bundleIdentifier"].as_string());

	return DeviceManager::instance()->RemoveApp(bundleIdentifier, udid)
		.then([=](pplx::task<void> task) {
		try
		{
			task.get();

			auto response = json::value::object();
			response[L"version"] = json::value::number(1);
			response[L"identifier"] = json::value::string(L"RemoveAppResponse");
			return this->SendResponse(response);
		}
		catch (std::exception& exception)
		{
			throw;
		}
	});
}

pplx::task<void> ClientConnection::ProcessEnableUnsignedCodeExecutionRequest(web::json::value request)
{
    return pplx::create_task([this, request]() {

        auto udid = StringFromWideString(request.at(L"udid").as_string());

        std::shared_ptr<Device> device = NULL;
        for (auto& d : DeviceManager::instance()->availableDevices())
        {
            if (d->identifier() == udid)
            {
                device = d;
                break;
            }
        }

        if (device == NULL)
        {
            throw ServerError(ServerErrorCode::DeviceNotFound);
        }

        return MiniappBuilderCore::instance()->PrepareDevice(device)
        .then([request, device](void) {
            return DeviceManager::instance()->StartDebugConnection(device);
        })
        .then([request, device](std::shared_ptr<DebugConnection> connection) {

            if (request.has_integer_field(L"processID"))
            {
                auto pid = request.at(L"processID").as_integer();
                return connection->EnableUnsignedCodeExecution(pid).then([connection](void) {
                    connection->Disconnect();
                });
            }
            else
            {
                auto processName = StringFromWideString(request.at(L"processName").as_string());
                return connection->EnableUnsignedCodeExecution(processName).then([connection](void) {
                    connection->Disconnect();
                });
            }            
        });
    })
    .then([=](pplx::task<void> task) {
        try
        {
            task.get();

            auto response = json::value::object();
            response[L"version"] = json::value::number(1);
            response[L"identifier"] = json::value::string(L"EnableUnsignedCodeExecutionResponse");
            return this->SendResponse(response);
        }
        catch (std::exception& exception)
        {
            throw;
        }
    });
}

web::json::value ClientConnection::ErrorResponse(std::exception& exception)
{
	auto response = json::value::object();
	response[L"version"] = json::value::number(2);
	response[L"identifier"] = json::value::string(L"ErrorResponse");

	auto errorObject = json::value::object();

	try
	{
        try 
        {
            ServerError& error = dynamic_cast<ServerError&>(exception);

            response[L"errorCode"] = json::value::number(error.code());
            errorObject[L"errorCode"] = json::value::number(error.code());

            if (!error.userInfo().empty())
            {
                auto userInfo = json::value::object();

                for (auto& pair : error.userInfo())
                {
                    userInfo[WideStringFromString(pair.first)] = json::value(WideStringFromString(pair.second));
                }

                errorObject[L"userInfo"] = userInfo;
            }
        }
        catch (std::bad_cast)
        {
            Error& error = dynamic_cast<Error&>(exception);

            response[L"errorCode"] = json::value::number((int)ServerErrorCode::UnderlyingError);
            errorObject[L"errorCode"] = json::value::number((int)ServerErrorCode::UnderlyingError);

            auto userInfo = json::value::object();
            userInfo[L"NSLocalizedDescription"] = json::value(WideStringFromString(error.localizedDescription()));

            for (auto& pair : error.userInfo())
            {
                userInfo[WideStringFromString(pair.first)] = json::value(WideStringFromString(pair.second));
            }

            errorObject[L"userInfo"] = userInfo;
        }
	}
	catch (std::bad_cast)
	{
		response[L"errorCode"] = json::value::number((int)ServerErrorCode::Unknown);
		errorObject[L"errorCode"] = json::value::number((int)ServerErrorCode::Unknown);

		auto userInfo = json::value::object();

		if (std::string(exception.what()) == "vector<T> too long")
		{
			userInfo[L"NSLocalizedFailureReason"] = json::value::string(L"Windows Defender Blocked Installation");
			userInfo[L"NSLocalizedRecoverySuggestion"] = json::value::string(L"Disable Windows real-time protection on your computer then try again.");
		}
		else
		{
			userInfo[L"NSLocalizedDescription"] = json::value::string(WideStringFromString(exception.what()));
			userInfo[L"NSLocalizedFailureReason"] = json::value::string(WideStringFromString(exception.what()));
		}
		
		errorObject[L"userInfo"] = userInfo;
	}

	response[L"serverError"] = errorObject;

	return response;
}

pplx::task<void> ClientConnection::SendResponse(web::json::value json)
{
	auto serializedJSON = json.serialize();
	std::vector<unsigned char> responseData(serializedJSON.begin(), serializedJSON.end());

	int32_t size = (int32_t)responseData.size();

	std::vector<unsigned char> responseSizeData;

	if (responseSizeData.size() < sizeof(size))
	{
		responseSizeData.resize(sizeof(size));
	}

	std::memcpy(responseSizeData.data(), &size, sizeof(size));

	std::cout << "Represented Value: " << *((int32_t*)responseSizeData.data()) << std::endl;

	auto task = this->SendData(responseSizeData)
	.then([this, responseData]() mutable {
		return this->SendData(responseData);
	})
	.then([](pplx::task<void> task) {
		try
		{
			task.get();
		}
		catch (Error& error)
		{
			odslog("Failed to send response. " << error.localizedDescription());
			throw;
		}
		catch (std::exception& exception)
		{
			odslog("Failed to send response. " << exception.what());
			throw;
		}
	});

	return task;
}

pplx::task<web::json::value> ClientConnection::ReceiveRequest()
{
	int size = sizeof(uint32_t);

	std::cout << "Receiving request size..." << std::endl;

	auto task = this->ReceiveData(size)
	.then([this](std::vector<unsigned char> data) {
		int expectedBytes = *((int32_t*)data.data());
		std::cout << "Receiving " << expectedBytes << " bytes..." << std::endl;

		return this->ReceiveData(expectedBytes);
	})
	.then([](std::vector<unsigned char> data) {
		std::wstring jsonString(data.begin(), data.end());

		auto request = web::json::value::parse(jsonString);
		return request;
	});

	return task;
}
