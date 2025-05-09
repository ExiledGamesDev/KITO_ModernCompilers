#include "RakString.h"
#include "AutopatcherMySQLRepository.h"
#include "AutopatcherPatchContext.h"
#include "FileList.h"
#include "RakAssert.h"
#include "DS_List.h"
// ntohl
#ifdef _WIN32
#include <Winsock2.h>
#else
#include <netinet/in.h>
#endif

#include "mysql.h"
#include "CreatePatch.h"
#include "AutopatcherPatchContext.h"
// #include "SHA1.h"
#include <stdlib.h>
#include "LinuxStrings.h"

static const unsigned HASH_LENGTH=sizeof(unsigned int);

struct FileInfo
{	
	RakNet::RakString filename;
	char contentHash [HASH_LENGTH];
	bool createFile;
};

// alloca
#ifdef _COMPATIBILITY_1
#elif defined(_WIN32)
#include <malloc.h>
#else
//#include <stdlib.h>
#endif


#define PQEXECPARAM_FORMAT_TEXT		0
#define PQEXECPARAM_FORMAT_BINARY	1

AutopatcherMySQLRepository::AutopatcherMySQLRepository()
{
}

AutopatcherMySQLRepository::~AutopatcherMySQLRepository()
{
}
bool AutopatcherMySQLRepository::CreateAutopatcherTables(void)
{
	if (!IsConnected())
		return false;

	ExecuteBlockingCommand("BEGIN;");

	if (!ExecuteBlockingCommand(
		"CREATE TABLE Applications ("
		"applicationID INT AUTO_INCREMENT,"
		"applicationName VARCHAR(255) NOT NULL UNIQUE,"
		"changeSetID integer NOT NULL DEFAULT 0,"
		"userName TEXT NOT NULL,"
		"PRIMARY KEY (applicationID));"
		)) {Rollback(); return false;}

	if (!ExecuteBlockingCommand(
		"CREATE TABLE FileVersionHistory ("
		"fileID INT AUTO_INCREMENT,"
		"applicationID INT NOT NULL, "
		"filename TEXT NOT NULL,"
		"fileLength  INT,"
		"content LONGBLOB,"
		"contentHash TINYBLOB,"
		"patch LONGBLOB,"
		"createFile TINYINT NOT NULL,"
		"modificationDate TIMESTAMP NOT NULL DEFAULT LOCALTIMESTAMP,"
		"lastSentDate TIMESTAMP,"
		"timesSent INT NOT NULL DEFAULT 0,"
		"changeSetID INT NOT NULL,"
		"userName TEXT NOT NULL,"
		"PRIMARY KEY (fileID)); "
		)) {Rollback(); return false;}

	if (!ExecuteBlockingCommand(
		"CREATE VIEW AutoPatcherView AS SELECT "
		"FileVersionHistory.applicationid,"
		"Applications.applicationName,"
		"FileVersionHistory.fileID,"
		"FileVersionHistory.fileName,"
		"FileVersionHistory.createFile,"
		"FileVersionHistory.fileLength,"
		"FileVersionHistory.changeSetID,"
		"FileVersionHistory.lastSentDate," 
		"FileVersionHistory.modificationDate,"
		"FileVersionHistory.timesSent "
		"FROM (FileVersionHistory JOIN Applications ON "
		"( FileVersionHistory.applicationID = Applications.applicationID )) "
		"ORDER BY Applications.applicationID ASC, FileVersionHistory.fileID ASC;"
		)) {Rollback(); return false;}

	return ExecuteBlockingCommand("COMMIT;");
}

bool AutopatcherMySQLRepository::DestroyAutopatcherTables(void)
{
	if (!IsConnected())
		return false;

	ExecuteBlockingCommand("DROP TABLE Applications CASCADE;");
	ExecuteBlockingCommand("DROP TABLE FileVersionHistory CASCADE;");
	return ExecuteBlockingCommand("DROP VIEW AutoPatcherView;");
}

bool AutopatcherMySQLRepository::AddApplication(const char *applicationName, const char *userName)
{
	// mysql_real_escape_string

	char query[512];
	sprintf(query, "INSERT INTO Applications (applicationName, userName) VALUES ('%s', '%s');", GetEscapedString(applicationName).C_String(), GetEscapedString(userName).C_String());
	return ExecuteBlockingCommand(query);
}
bool AutopatcherMySQLRepository::RemoveApplication(const char *applicationName)
{
	char query[512];	
	sprintf(query, "DELETE FROM Applications WHERE applicationName='%s';", GetEscapedString(applicationName).C_String());
	return ExecuteBlockingCommand(query);
}

bool AutopatcherMySQLRepository::GetChangelistSinceDate(const char *applicationName, FileList *addedFiles, FileList *deletedFiles, const char *sinceDate, char currentDate[64])
{
	char query[512];
	if (sinceDate != 0 && strlen(sinceDate)>63)
		return false;
	RakNet::RakString escapedApplicationName = GetEscapedString(applicationName);
	RakNet::RakString escapedSinceDate = GetEscapedString(sinceDate);
	sprintf(query, "SELECT applicationID FROM Applications WHERE applicationName='%s';", escapedApplicationName.C_String());

	int applicationID;
	if (!ExecuteQueryReadInt(query, &applicationID))
	{
		sprintf(lastError,"ERROR: %s not found in UpdateApplicationFiles\n",escapedApplicationName.C_String());
		return false;
	}

	if (sinceDate && sinceDate[0])
		sprintf(query,
		"SELECT filename, fileLength, contentHash, createFile, fileId FROM FileVersionHistory "
		"JOIN (SELECT max(fileId) maxId FROM FileVersionHistory WHERE applicationId=%i AND modificationDate > '%s' GROUP BY fileName) MaxId "
		"ON FileVersionHistory.fileId = MaxId.maxId "
		"ORDER BY filename DESC;", applicationID,escapedSinceDate.C_String());
	else
		sprintf(query,
		"SELECT filename, fileLength, contentHash, createFile, fileId FROM FileVersionHistory "
		"JOIN (SELECT max(fileId) maxId FROM FileVersionHistory WHERE applicationId=%i GROUP BY fileName) MaxId "
		"ON FileVersionHistory.fileId = MaxId.maxId "
		"ORDER BY filename DESC;", applicationID);

	MYSQL_RES * result = 0;
	if (!ExecuteBlockingCommand (query, &result))
		return false;

	MYSQL_ROW row;
	while ((row = mysql_fetch_row (result)) != 0)
	{
		const char * createFileResult = row [3]; 
		const char * hardDriveFilename = row [0];
		if (createFileResult[0]=='1')
		{
			const char * hardDriveHash = row [2]; 
			int fileLength = atoi (row [1]);
			addedFiles->AddFile(hardDriveFilename, hardDriveHash, HASH_LENGTH, fileLength, 0);
		}
		else
		{
			deletedFiles->AddFile(hardDriveFilename,0,0,0,0);
		}
	}
	mysql_free_result (result);

	char *localTimestamp =GetLocalTimestamp ();
	if (localTimestamp)
		strcpy(currentDate, localTimestamp);;

	return true;
}

bool AutopatcherMySQLRepository::GetPatches(const char *applicationName, FileList *input, FileList *patchList, char currentDate[64])
{
	char query[512];
	RakNet::RakString escapedApplicationName = GetEscapedString(applicationName);
	sprintf(query, "SELECT applicationID FROM Applications WHERE applicationName='%s';", escapedApplicationName.C_String());
	int applicationID;
	if (!ExecuteQueryReadInt (query, &applicationID))
	{
	    sprintf(lastError,"ERROR: %s not found in GetPatches\n",applicationName);
	    return false;
	}

	// Go through the input list.
	for (unsigned inputIndex=0; inputIndex < input->fileList.Size(); inputIndex++)
	{
		const char * userHash=input->fileList[inputIndex].data;
		const char * userFilename=input->fileList[inputIndex].filename;

		char *fn = new char [(strlen(userFilename))*2+1];
		mysql_real_escape_string(mySqlConnection, fn, userFilename, (unsigned long) strlen(userFilename));

		if (userHash==0)
		{
			// If the user does not have a hash in the input list, get the contents of latest version of this named file and write it to the patch list
			sprintf(query, "SELECT content FROM FileVersionHistory "
			               "JOIN (SELECT max(fileId) maxId FROM FileVersionHistory WHERE applicationId=%i AND filename='%s') MaxId "
			               "ON FileVersionHistory.fileId = MaxId.maxId", 
				applicationID, fn);

			MYSQL_RES * result = 0;
			if (!ExecuteBlockingCommand (query, &result))
			{
				delete [] fn;
				return false;
			}

			MYSQL_ROW row = mysql_fetch_row (result);	
			if (row != 0)
			{
				const char * content = row [0];
				unsigned long contentLength=mysql_fetch_lengths (result) [0];
				patchList->AddFile(userFilename, content, contentLength, contentLength, PC_WRITE_FILE);
			}
			mysql_free_result(result);
		}
		else // Assuming the user does have a hash.
		{
			if (input->fileList[inputIndex].dataLengthBytes!=HASH_LENGTH)
			{
				delete [] fn;
				return false;
			}

			// Get the hash and ID of the latest version of this file, by filename.
			sprintf(query, 
				"SELECT contentHash, fileId, fileLength FROM FileVersionHistory "
				"JOIN (SELECT max(fileId) maxId FROM FileVersionHistory WHERE applicationId=%i AND filename='%s') MaxId "
				"ON FileVersionHistory.fileId = MaxId.maxId",
				applicationID, fn);

			MYSQL_RES * result = 0;
			if (!ExecuteBlockingCommand (query, &result))
			{
				delete [] fn;
				return false;
			}

			MYSQL_ROW row = mysql_fetch_row (result);	
			if (row != 0)
			{
				const char * contentHash = row [0];
				const int fileId = atoi (row [1]); 
				const int fileLength = atoi (row [2]);   // double check if this works

				if (memcmp(contentHash, userHash, HASH_LENGTH)!=0)
				{
					char buf [2 * HASH_LENGTH + 1];
					mysql_real_escape_string(mySqlConnection, buf, userHash, HASH_LENGTH);
					
					sprintf(query, "SELECT patch FROM FileVersionHistory WHERE applicationId=%i AND filename='%s' AND contentHash='%s'; ", applicationID, fn, buf);
                    MYSQL_RES * patchResult = 0;
                    if (!ExecuteBlockingCommand (query, &patchResult))
					{
						delete [] fn;
                        return false;
					}

                    MYSQL_ROW row = mysql_fetch_row (patchResult);
					if (row==0)
					{
						// If no patch found, then this is a non-release version, or a very old version we are no longer tracking.
						// Get the contents of latest version of this named file by fileId and return it.
						sprintf(query, "SELECT content FROM FileVersionHistory WHERE fileId=%d;", fileId);

						if (mysql_query (mySqlConnection, query) != 0)
						{
							delete [] fn;
							strcpy (lastError, mysql_error (mySqlConnection));
							return false;
						}

						// Possible TODO - Get 32000000 bytes at a time to workaround http://support.microsoft.com/kb/q201213
						MYSQL_RES * substrresult = mysql_store_result (mySqlConnection);
						MYSQL_ROW row = mysql_fetch_row (substrresult);
						char * file = row [0];
						unsigned long contentLength = mysql_fetch_lengths (substrresult) [0];
						
						patchList->AddFile(userFilename, file, fileLength, contentLength, PC_WRITE_FILE);
						mysql_free_result(substrresult);
					}
					else
					{
						// Otherwise, write the hash of the new version and then write the patch to get to that version.
						// 
						char * patch = row [0];
						unsigned long patchLength = mysql_fetch_lengths (patchResult) [0];

						char *temp = new char [patchLength + HASH_LENGTH];
						memcpy(temp, contentHash, HASH_LENGTH);
						memcpy(temp+HASH_LENGTH, patch, patchLength);

						patchList->AddFile(userFilename, temp, HASH_LENGTH+patchLength, fileLength, PC_HASH_WITH_PATCH);
						delete [] temp;
					}

					mysql_free_result(patchResult);

				}
				else
				{
					// else if the hash of this file matches what the user has, the user has the latest version.  Done.
				}
			}
			else
			{
				// else if there is no such file, skip this file.
			}
			mysql_free_result(result);
		}

		delete [] fn;
	}

	char *localTimestamp =GetLocalTimestamp ();
	if (localTimestamp)
		strcpy(currentDate, localTimestamp);;

	return true;
}

bool AutopatcherMySQLRepository::UpdateApplicationFiles(const char *applicationName, const char *applicationDirectory, const char *userName, FileListProgress *cb)
{
	MYSQL_STMT    *stmt;
	MYSQL_BIND    bind[3];
	char query[512];
	FileList filesOnHarddrive;
	filesOnHarddrive.SetCallback(cb);
	int prepareResult;
	my_bool falseVar=false;
	RakNet::RakString escapedApplicationName = GetEscapedString(applicationName);
	filesOnHarddrive.AddFilesFromDirectory(applicationDirectory,"", true, true, true, 0);
	if (filesOnHarddrive.fileList.Size()==0)
	{
		sprintf(lastError,"ERROR: Can't find files at %s in UpdateApplicationFiles\n",applicationDirectory);
		return false;
	}

	sprintf(query, "SELECT applicationID FROM Applications WHERE applicationName='%s';", escapedApplicationName.C_String());
	int applicationID;

	if (!ExecuteQueryReadInt(query, &applicationID))
	{
		sprintf(lastError,"ERROR: %s not found in UpdateApplicationFiles\n",escapedApplicationName.C_String());
		return false;
	}

	if (!ExecuteBlockingCommand("BEGIN;"))
	{
		return false;
	}
	sprintf(query, "UPDATE Applications SET changeSetId = changeSetId + 1 where applicationID=%i;", applicationID);
	if (!ExecuteBlockingCommand(query))
	{
		Rollback ();
		return false;
	}
	int changeSetId = 0;
	sprintf(query, "SELECT changeSetId FROM Applications WHERE applicationID=%i;", applicationID);
	if (!ExecuteQueryReadInt(query, &changeSetId))
	{
		Rollback ();
		return false;
	}

	// +1 was added in the update
	changeSetId--;

	// Gets all newest files
	sprintf(query, "SELECT filename, contentHash, createFile FROM FileVersionHistory "
	               "JOIN (SELECT max(fileId) maxId FROM FileVersionHistory WHERE applicationId=%i GROUP BY fileName) MaxId "
	               "ON FileVersionHistory.fileId = MaxId.maxId "
	               "ORDER BY filename DESC;", applicationID);

	MYSQL_RES *result = 0;
	if (!ExecuteBlockingCommand(query, &result))
	{
		Rollback();
		return false;
	}
	DataStructures::List <FileInfo> newestFiles;
	MYSQL_ROW row;
	while ((row = mysql_fetch_row (result)) != 0)
	{
	    FileInfo fi;
		fi.filename = row [0];
		fi.createFile = (atoi (row [2]) != 0);
		if (fi.createFile)
		{
			RakAssert(mysql_fetch_lengths (result) [1] == HASH_LENGTH);  // check the data is sensible
			memcpy (fi.contentHash, row [1], HASH_LENGTH);
		}
	    newestFiles.Insert (fi);
	}    
	mysql_free_result(result);


	FileList newFiles;
	// Loop through files on filesOnHarddrive
	// If the file in filesOnHarddrive does not exist in the query result, or if it does but the hash is different or non-existent, add this file to the create list
	for (unsigned fileListIndex=0; fileListIndex < filesOnHarddrive.fileList.Size(); fileListIndex++)
	{
		bool addFile=true;
		if (fileListIndex%10==0)
			printf("Hashing files %i/%i\n", fileListIndex+1, filesOnHarddrive.fileList.Size());

		const char * hardDriveFilename=filesOnHarddrive.fileList[fileListIndex].filename;
		const char * hardDriveHash=filesOnHarddrive.fileList[fileListIndex].data;

		for (unsigned i = 0; i != newestFiles.Size (); ++i)
		{
			const FileInfo & fi = newestFiles [i];
                        
			if (_stricmp(hardDriveFilename, fi.filename)==0)
			{
				if (fi.createFile && memcmp(fi.contentHash, hardDriveHash, HASH_LENGTH)==0)
				{
					// File exists in database and is the same
					addFile=false;
				}

				break;
			}
		}

		// Unless set to false, file does not exist in query result or is different.
		if (addFile)
		{
			newFiles.AddFile(hardDriveFilename, filesOnHarddrive.fileList[fileListIndex].data, filesOnHarddrive.fileList[fileListIndex].dataLengthBytes, filesOnHarddrive.fileList[fileListIndex].fileLengthBytes, 0);
		}
	}
	
	// Go through query results that are marked as create
	// If a file that is currently in the database is not on the harddrive, add it to the delete list
	FileList deletedFiles;
	for (unsigned i = 0; i != newestFiles.Size (); ++i)
	{
		const FileInfo & fi = newestFiles [i];
		if (!fi.createFile)
			continue; // If already false don't mark false again.

		bool fileOnHarddrive=false;
		for (unsigned fileListIndex=0; fileListIndex < filesOnHarddrive.fileList.Size(); fileListIndex++)
		{
			const char * hardDriveFilename=filesOnHarddrive.fileList[fileListIndex].filename;
			//hardDriveHash=filesOnHarddrive.fileList[fileListIndex].data;

			if (_stricmp(hardDriveFilename, fi.filename)==0)
			{
				fileOnHarddrive=true;
				break;
			}
		}

		if (!fileOnHarddrive)
			deletedFiles.AddFile(fi.filename,0,0,0,0);
	}

	// files on harddrive no longer needed.  Free this memory since generating all the patches is memory intensive.
	filesOnHarddrive.Clear();

	// For each file in the delete list add a row indicating file deletion
	for (unsigned fileListIndex=0; fileListIndex < deletedFiles.fileList.Size(); fileListIndex++)
	{
		if (fileListIndex%10==0)
			printf("Tagging deleted files %i/%i\n", fileListIndex+1, deletedFiles.fileList.Size());

		sprintf(query, "INSERT INTO FileVersionHistory(applicationID, filename, createFile, changeSetID, userName) VALUES (%i, '%s', FALSE,%i,'%s');", 
			applicationID, GetEscapedString(deletedFiles.fileList[fileListIndex].filename).C_String(), changeSetId, GetEscapedString(userName).C_String());
		
		if (!ExecuteBlockingCommand (query))
		{
			Rollback();
			deletedFiles.Clear();
			newFiles.Clear();
			return false;
		}
	}
	
	// Clear the delete list as it is no longer needed.
	deletedFiles.Clear();

	// For each file in the create list
	for (unsigned fileListIndex=0; fileListIndex < newFiles.fileList.Size(); fileListIndex++)
	{
		if (fileListIndex%10==0)
			printf("Adding file %i/%i\n", fileListIndex+1, newFiles.fileList.Size());
		const char * hardDriveFilename=newFiles.fileList[fileListIndex].filename;
		const char * hardDriveData=newFiles.fileList[fileListIndex].data+HASH_LENGTH;
		const char * hardDriveHash=newFiles.fileList[fileListIndex].data;
		unsigned     hardDriveDataLength=newFiles.fileList[fileListIndex].fileLengthBytes;

		sprintf( query, "SELECT fileID from FileVersionHistory WHERE applicationID=%i AND filename='%s' AND createFile=TRUE;", applicationID, GetEscapedString(hardDriveFilename).C_String() );

		MYSQL_RES * res = 0;
		if (!ExecuteBlockingCommand (query, &res))
		{
			Rollback();
			newFiles.Clear();
			return false;
		}
		
		// Create new patches for every create version
		MYSQL_ROW row;

		while ((row = mysql_fetch_row (res)) != 0)
		{
			const char * fileID = row [0];
			
			// The last query handled all the relevant comparisons
			sprintf(query, "SELECT content from FileVersionHistory WHERE fileID=%s", fileID );
			MYSQL_RES * queryResult = 0;
			if (!ExecuteBlockingCommand (query, &queryResult))
			{
				Rollback();
				newFiles.Clear();
				mysql_free_result(res);
				return false;
			}
		
			MYSQL_ROW queryRow = mysql_fetch_row (queryResult);

			const unsigned contentLength=mysql_fetch_lengths (queryResult) [0];
			const char * content=queryRow [0];

			char *patch;
			unsigned patchLength;	
			if (!CreatePatch(content, contentLength, (char *) hardDriveData, hardDriveDataLength, &patch, &patchLength))
			{
				strcpy(lastError,"CreatePatch failed.");
				Rollback();

				newFiles.Clear();
				mysql_free_result(res);
				mysql_free_result(queryResult);
				return false;
			}
			
			char buf[512];
			stmt = mysql_stmt_init(mySqlConnection);
			sprintf (buf, "UPDATE FileVersionHistory SET patch=? where fileID=%s;", fileID);
			if ((prepareResult=mysql_stmt_prepare(stmt, buf, (unsigned long) strlen(buf)))!=0)
			{
				strcpy (lastError, mysql_stmt_error (stmt));
				mysql_stmt_close(stmt);
				Rollback();
				return false;
			}
			memset(bind, 0, sizeof(bind));

			unsigned long l1;
			l1=patchLength;
			bind[0].buffer_type= MYSQL_TYPE_LONG_BLOB;
			bind[0].buffer= patch;
			bind[0].buffer_length= patchLength;
			bind[0].is_null= &falseVar;
			bind[0].length=&l1;

			if (mysql_stmt_bind_param(stmt, bind))
			{
				strcpy (lastError, mysql_stmt_error (stmt));
				mysql_stmt_close(stmt);
				Rollback();
				return false;
			}

			if (mysql_stmt_execute(stmt))
			{
				strcpy (lastError, mysql_stmt_error (stmt));
				mysql_stmt_close(stmt);
				Rollback();
				newFiles.Clear();
				mysql_free_result(res);
				mysql_free_result(queryResult);
				delete [] patch;
				return false;
			}

			mysql_stmt_close(stmt);
			delete [] patch;

			mysql_free_result(queryResult);
		}
         mysql_free_result(res);

		 stmt = mysql_stmt_init(mySqlConnection);
		 sprintf(query, "INSERT INTO FileVersionHistory (applicationID, filename, fileLength, content, contentHash, createFile, changeSetID, userName) "
			 "VALUES (%i, ?, %i,?,?, TRUE, %i, '%s' );", 
			 applicationID, hardDriveDataLength, changeSetId, GetEscapedString(userName).C_String());

		 if ((prepareResult=mysql_stmt_prepare(stmt, query, (unsigned long) strlen(query)))!=0)
		 {
			 strcpy (lastError, mysql_stmt_error (stmt));
			 mysql_stmt_close(stmt);
			 Rollback();
			 return false;
		 }
		 memset(bind, 0, sizeof(bind));

		 unsigned long l2,l3,l4;
		 l2=(unsigned long) strlen(hardDriveFilename);
		 l3=hardDriveDataLength;
		 l4=HASH_LENGTH;
		 bind[0].buffer_type= MYSQL_TYPE_STRING;
		 bind[0].buffer= (void*) hardDriveFilename;
		 bind[0].buffer_length= (unsigned long) strlen(hardDriveFilename);
		 bind[0].is_null= &falseVar;
		 bind[0].length=&l2;

		 bind[1].buffer_type= MYSQL_TYPE_LONG_BLOB;
		 bind[1].buffer= (void*) hardDriveData;
		 bind[1].buffer_length= hardDriveDataLength;
		 bind[1].is_null= &falseVar;
		 bind[1].length=&l3;

		 bind[2].buffer_type= MYSQL_TYPE_TINY_BLOB;
		 bind[2].buffer= (void*) hardDriveHash;
		 bind[2].buffer_length= HASH_LENGTH;
		 bind[2].is_null= &falseVar;
		 bind[2].length=&l4;

		 if (mysql_stmt_bind_param(stmt, bind))
		 {
			 strcpy (lastError, mysql_stmt_error (stmt));
			 mysql_stmt_close(stmt);
			 Rollback();
			 return false;
		 }

		 if (mysql_stmt_execute(stmt))
		 {
			 strcpy (lastError, mysql_stmt_error (stmt));
			 mysql_stmt_close(stmt);
			 Rollback();
			 return false;
		 }

		 mysql_stmt_close(stmt);
	}

	if (!ExecuteBlockingCommand("COMMIT;"))
	{
        Rollback ();
		return false;
	}

	return true;
}

const char *AutopatcherMySQLRepository::GetLastError(void) const
{
	return MySQLInterface::GetLastError();
}
