//
//  Archiver.cpp
//  AltSign-Windows
//
//

#include <filesystem>
#include <fstream>

#include "Archiver.hpp"
#include "Error.hpp"

extern "C" {
#include "zip.h"
#include "unzip.h"
}

#ifdef _WIN32
#include <io.h>
#define access    _access_s
#else
#include <sys/stat.h>
#include <unistd.h>
 #include <dirent.h>
#endif

#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <cstdint>

const int ALTReadBufferSize = 8192;
const int ALTMaxFilenameLength = 512;

#include <sstream>

extern std::string StringFromWideString(std::wstring wideString);

#ifdef _WIN32
char ALTDirectoryDeliminator = '\\';
#else
char ALTDirectoryDeliminator = '/';
#endif

#define READ_BUFFER_SIZE 8192
#define MAX_FILENAME 512

namespace fs = std::filesystem;

static bool endsWith(const std::string& str, const std::string& suffix)
{
    return str.size() >= suffix.size() && 0 == str.compare(str.size()-suffix.size(), suffix.size(), suffix);
}

static bool startsWith(const std::string& str, const std::string& prefix)
{
    return str.size() >= prefix.size() && 0 == str.compare(0, prefix.size(), prefix);
}

extern std::string replace_all(
	const std::string& str,   // where to work
	const std::string& find,  // substitute 'find'
	const std::string& replace //      by 'replace'
);

void UnzipArchive(std::string archivePath, std::string outputDirectory)
{
	if (outputDirectory[outputDirectory.size() - 1] != ALTDirectoryDeliminator)
	{
		outputDirectory += ALTDirectoryDeliminator;
	}

	unzFile zipFile = unzOpen(archivePath.c_str());
	if (zipFile == NULL)
	{
		throw ArchiveError(ArchiveErrorCode::NoSuchFile);
	}

	FILE* outputFile = nullptr;

	auto finish = [&outputFile, &zipFile](void)
	{
		if (outputFile != nullptr)
		{
			fclose(outputFile);
		}

		unzCloseCurrentFile(zipFile);
		unzClose(zipFile);
	};

	unz_global_info zipInfo;
	if (unzGetGlobalInfo(zipFile, &zipInfo) != UNZ_OK)
	{
		finish();
		throw ArchiveError(ArchiveErrorCode::CorruptFile);
	}

	char buffer[ALTReadBufferSize];

	for (int i = 0; i < zipInfo.number_entry; i++)
	{
		unz_file_info info;
		char cFilename[ALTMaxFilenameLength];

		if (unzGetCurrentFileInfo(zipFile, &info, cFilename, ALTMaxFilenameLength, NULL, 0, NULL, 0) != UNZ_OK)
		{
			finish();
			throw ArchiveError(ArchiveErrorCode::Unknown);
		}

		std::string filename(cFilename);
		if (startsWith(filename, "__MACOSX"))
		{
			if (i + 1 < zipInfo.number_entry)
			{
				if (unzGoToNextFile(zipFile) != UNZ_OK)
				{
					finish();
					throw ArchiveError(ArchiveErrorCode::Unknown);
				}
			}

			continue;
		}

		std::replace(filename.begin(), filename.end(), '/', ALTDirectoryDeliminator);
		filename = replace_all(filename, ":", "__colon__");

		fs::path filepath = fs::path(outputDirectory).append(filename);
		fs::path parentDirectory = (filename[filename.size() - 1] == ALTDirectoryDeliminator) ? filepath.parent_path().parent_path() : filepath.parent_path();

		if (!fs::exists(parentDirectory))
		{
			fs::create_directories(parentDirectory);
		}

		if (filename[filename.size() - 1] == ALTDirectoryDeliminator)
		{
			// Directory
			fs::create_directories(filepath);
		}
		else
		{
			// File
			if (unzOpenCurrentFile(zipFile) != UNZ_OK)
			{
				finish();
				throw ArchiveError(ArchiveErrorCode::Unknown);
			}

			std::string narrowFilepath = StringFromWideString(filepath.c_str());

			outputFile = fopen(narrowFilepath.c_str(), "wb");
			if (outputFile == NULL)
			{
				finish();
				throw ArchiveError(ArchiveErrorCode::UnknownWrite);
			}

			int result = UNZ_OK;

			do
			{
				result = unzReadCurrentFile(zipFile, buffer, ALTReadBufferSize);

				if (result < 0)
				{
					finish();
					throw ArchiveError(ArchiveErrorCode::Unknown);
				}

				size_t count = fwrite(buffer, result, 1, outputFile);
				if (result > 0 && count != 1)
				{
					finish();
					throw ArchiveError(ArchiveErrorCode::UnknownWrite);
				}

			} while (result > 0);

			short permissions = (info.external_fa >> 16) & 0x01FF;
			_chmod(narrowFilepath.c_str(), permissions);

			fclose(outputFile);
			outputFile = NULL;
		}

		unzCloseCurrentFile(zipFile);

		if (i + 1 < zipInfo.number_entry)
		{
			if (unzGoToNextFile(zipFile) != UNZ_OK)
			{
				finish();
				throw ArchiveError(ArchiveErrorCode::Unknown);
			}
		}
	}

	finish();
}

std::string UnzipAppBundle(std::string ipaPath, std::string outputDirectory)
{
    if (outputDirectory[outputDirectory.size() - 1] != ALTDirectoryDeliminator)
    {
        outputDirectory += ALTDirectoryDeliminator;
    }

    UnzipArchive(ipaPath, outputDirectory);

	fs::path payloadDirectoryPath = fs::path(outputDirectory).append("Payload");
    for (auto & p : fs::directory_iterator(payloadDirectoryPath))
    {
        auto filename = p.path().filename().string();
        
        auto lowercaseFilename = filename;
        std::transform(lowercaseFilename.begin(), lowercaseFilename.end(), lowercaseFilename.begin(), [](unsigned char c) {
            return std::tolower(c);
        });
        
        if (!endsWith(lowercaseFilename, ".app"))
        {
            continue;
        }
        
        auto appBundlePath = payloadDirectoryPath;
        appBundlePath.append(filename);
        
        return appBundlePath.string();
    }
    
    throw SignError(SignError(SignErrorCode::MissingAppBundle));
}


void WriteFileToZipFile(zipFile *zipFile, fs::path filepath, fs::path relativePath)
{
    bool isDirectory = fs::is_directory(filepath);
    
    std::string filename = relativePath.string();
    
    zip_fileinfo fileInfo = {};
    
    char *bytes = nullptr;
    unsigned int fileSize = 0;
    
    if (isDirectory)
    {
        // Remove leading directory slash.
        if (filename[0] == ALTDirectoryDeliminator)
        {
            filename = std::string(filename.begin() + 1, filename.end());
        }
        
        // Add trailing directory slash.
        if (filename[filename.size() - 1] != ALTDirectoryDeliminator)
        {
            filename = filename + ALTDirectoryDeliminator;
        }
    }
    else
    {
        fs::file_status status = fs::status(filepath);
        
        short permissions = (short)status.permissions();
        long shiftedPermissions = 0100000 + permissions;
        
        uLong permissionsLong = (uLong)shiftedPermissions;
        
        fileInfo.external_fa = (unsigned int)(permissionsLong << 16L);
        
        // std::ifstream ifs(filepath.string());
        // std::vector<char> data((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        // bytes = data.data();
        // fileSize = (unsigned int)data.size();

		 std::fstream f(filepath.c_str(),std::ios::binary | std::ios::in);
		 f.seekg(0, std::ios::end);
		 fileSize = f.tellg();
		 f.seekg(0, std::ios::beg);
		 if ( fileSize <= 0 )
		 {
		 	 zipWriteInFileInZip(*zipFile, NULL, 0);
             return;
		 }
         bytes = new char[fileSize];
		 f.read(bytes, fileSize);
    }

	std::replace(filename.begin(), filename.end(), ALTDirectoryDeliminator, '/');
    
     if (zipOpenNewFileInZip(*zipFile, (const char *)filename.c_str(), &fileInfo, NULL, 0, NULL, 0, NULL, Z_DEFLATED, Z_DEFAULT_COMPRESSION) != ZIP_OK)
     {
         throw ArchiveError(ArchiveErrorCode::UnknownWrite);
     }
    
    if (zipWriteInFileInZip(*zipFile, bytes, fileSize) != ZIP_OK)
    {
        zipCloseFileInZip(*zipFile);
        throw ArchiveError(ArchiveErrorCode::UnknownWrite);
    }
	delete[] bytes;
}

std::string ZipAppBundle(std::string appBundleFilePath)
{
    fs::path appBundlePath = appBundleFilePath;
    
    auto appBundleFilename = appBundlePath.filename();
    auto appName = appBundlePath.filename().stem().string();
    auto ipaName = appName + ".ipa";
    auto ipaPath = appBundlePath.remove_filename().append(ipaName);
    
    if (fs::exists(ipaPath))
    {
        fs::remove(ipaPath);
    }
    
    zipFile zipFile = zipOpen((const char *)ipaPath.string().c_str(), APPEND_STATUS_CREATE);
    if (zipFile == nullptr)
    {
        throw ArchiveError(ArchiveErrorCode::UnknownWrite);
    }
    
    fs::path payloadDirectory = "Payload";
    fs::path appBundleDirectory = payloadDirectory.append(appBundleFilename.string());
	fs::path payloadAbsDirectory = appBundlePath.parent_path().parent_path();
    
    for (auto& entry: fs::recursive_directory_iterator(appBundleFilePath))
    {
        auto filepath = entry.path();
        auto relativePath = fs::relative(filepath, payloadAbsDirectory);
        
        WriteFileToZipFile(&zipFile, filepath, relativePath);
    }
    
   /* WriteFileToZipFile(&zipFile, payloadDirectory, payloadDirectory);
    WriteFileToZipFile(&zipFile, appBundleDirectory, appBundleDirectory);*/
    
    zipClose(zipFile, NULL);

    
    return ipaPath.string();
}

