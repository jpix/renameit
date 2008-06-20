#include "StdAfx.h"
#include "RenamingList.h"
#include "../resource.h"
#include "ScopedLocale.h"
#include "IOOperation/CreateDirectoryOperation.h"
#include "IOOperation/RenameOperation.h"
#include "IOOperation/RemoveEmptyDirectoryOperation.h"

using namespace Beroux::IO::Renaming::IOOperation;

namespace Beroux{ namespace IO{ namespace Renaming
{

CRenamingList::CRenamingList(void)
	: m_nWarnings(0)
	, m_nErrors(0)
{
	m_fOnProgress = bind(&CRenamingList::DefaultProgressCallback, this, _1, _2, _3);
}

CRenamingList::CRenamingList(const CFileList& flBefore, const CFileList& flAfter)
	: m_nWarnings(0)
	, m_nErrors(0)
{
	Create(flBefore, flAfter);
}

CRenamingList::~CRenamingList(void)
{
}

void CRenamingList::Create(const CFileList& flBefore, const CFileList& flAfter)
{
	if (flBefore.GetFileCount() != flAfter.GetFileCount())
		throw logic_error("The number of files before and after renaming must be the same.");

	// Combine the two lists into a renaming list.
	m_vRenamingOperations.resize(flBefore.GetFileCount());
	m_vProblems.resize(flBefore.GetFileCount());
	for (int i=0; i<flBefore.GetFileCount(); ++i)
		m_vRenamingOperations[i] = CRenamingOperation(flBefore[i], flAfter[i]);
}

bool CRenamingList::Check()
{
	// Pre-conditions
#ifdef _DEBUG
	BOOST_FOREACH(CRenamingOperation& operation, m_vRenamingOperations)
	{
		// Each operation must be a Unicode path.
		ASSERT(operation.pathBefore.GetPath().Left(4) == "\\\\?\\");
		ASSERT(operation.pathAfter.GetPath().Left(4) == "\\\\?\\");

		// Each operation changes something.
		ASSERT(operation.pathBefore != operation.pathAfter);
	}
#endif

	// Declarations.
	const unsigned nFilesCount = (unsigned)m_vRenamingOperations.size();
	if (nFilesCount == 0)
		return true;	// Nothing to rename.

	// Reinitialize the error list.
	ASSERT(m_vProblems.size() == m_vRenamingOperations.size());
	m_nErrors = m_nWarnings = 0;
	BOOST_FOREACH(COperationProblem& problem, m_vProblems)
	{
		problem.nErrorLevel = levelNone;
		problem.nErrorCode = errNoError;
		problem.strMessage.Empty();
	}

	// Change the locale to match the file system stricmp().
	CScopedLocale scopeLocale(_T(""));
	
	{
		// First pass: Preparation.
		set<CString> setBeforeLower;
		for (unsigned i=0; i<nFilesCount; ++i)
		{
			// Report progress
			OnProgress(stageChecking, i*20/nFilesCount, 100);

			// Create a map of file names (in lower case) associated to the operation index.
			CString strName = m_vRenamingOperations[i].pathBefore.GetPath();
			setBeforeLower.insert( strName.MakeLower() );
		}	
	
		// Check folders' case consistency (1/2): Find the length of the shortest path.
		int nMinAfterDirIndex = FindShortestDirectoryPathAfter(m_vRenamingOperations);

		map<CString, DIR_CASE, dir_case_compare> mapDirsCase;
		{
			// Insert the shortest path to the map (to improve a little the speed).
			CString strShortestDirAfter = m_vRenamingOperations[nMinAfterDirIndex].pathAfter.GetDirectoryName();
			mapDirsCase.insert( pair<CString, DIR_CASE>(
				strShortestDirAfter.Left(strShortestDirAfter.GetLength() - 1),
				DIR_CASE(nMinAfterDirIndex, strShortestDirAfter.GetLength())) );
		}

		// Checking loop.
		map<CString, int> mapAfterLower;
		for (unsigned i=0; i<nFilesCount; ++i)
		{
			// Report progress
			OnProgress(stageChecking, 20 + i*80/nFilesCount, 100);
	
			// Check for file conflicts.
			CheckFileConflict(i, setBeforeLower, mapAfterLower);

			// Check if the file/folder still exists
			if (!CPath::PathFileExists( m_vRenamingOperations[i].pathBefore.GetPath() ))
			{
				CString strErrorMsg;
				strErrorMsg.LoadString(IDS_REMOVED_FROM_DISK);
				SetProblem(i, errFileMissing, strErrorMsg);
			}

			// Check the file/folder name.
			COperationProblem problem = CheckName(
				m_vRenamingOperations[i].pathAfter.GetFileName(),
				m_vRenamingOperations[i].pathAfter.GetFileNameWithoutExtension(),
				true);
			if (problem.nErrorLevel != levelNone)
			{
				// Some problem found.
				SetProblem(i, problem.nErrorCode, problem.strMessage);
			}

			// Check the directory name.
			CheckDirectoryPath(i);

			// Check folders' case consistency 2/2
			{
				// Create a copy of the folder name.
				CString strDirAfter = m_vRenamingOperations[i].pathAfter.GetDirectoryName();
				ASSERT(strDirAfter[strDirAfter.GetLength() - 1] == '\\');
				strDirAfter = strDirAfter.Left(strDirAfter.GetLength() - 1);

				// For each parent directory name (starting from GetDirectoryName()).
				const int nMinDirAfterLength = m_vRenamingOperations[nMinAfterDirIndex].pathAfter.GetDirectoryName().GetLength() - 1; // note we don't count the last '\' since we remove it.
				while (strDirAfter.GetLength() >= nMinDirAfterLength)
				{
					// If the directory is in the map.
					map<CString, DIR_CASE, dir_case_compare>::iterator iter = mapDirsCase.find(strDirAfter);
					if (iter == mapDirsCase.end())
					{
						// Add it to the map.
						mapDirsCase.insert( pair<CString, DIR_CASE>(
							strDirAfter,
							DIR_CASE(i, strDirAfter.GetLength())) );
					}
					else
					{
						// Check if the case differ.
						if (iter->first != strDirAfter)
						{
							// Report the problem.
							CString strMessage;
							strMessage.LoadString(IDS_INCONSISTENT_DIRECTORY_CASE);
							SetProblem(i, errDirCaseInconsistent, strMessage);
						} // end: Check if the case differ.

						// Add the RO's index.
						iter->second.vnOperationsIndex.push_back( i );
					} // end if the directory is in the map.

					// Go to the next parent folder.
					int nPos = strDirAfter.ReverseFind('\\');
					if (nPos == -1)
						break;
					strDirAfter = strDirAfter.Left(nPos);
				} // end while.
			} // end of case consistency check.
		} // end: checking loop.
	}

	// Post condition.
	ASSERT(m_vProblems.size() == m_vRenamingOperations.size());

	// Report the errors.
	return m_nErrors == 0 && m_nWarnings == 0;
}

void CRenamingList::CheckFileConflict(int nOperationIndex, const set<CString>& setBeforeLower, map<CString, int>& mapAfterLower)
{
	// If that file isn't already marked as conflicting with another,
	// test if it's going to conflict with another file.
	if (m_vProblems[nOperationIndex].nErrorCode == errConflict)
		return;

	CString strAfterLower = m_vRenamingOperations[nOperationIndex].pathAfter.GetPath();
	strAfterLower.MakeLower();
	
	// Detect if it's going to be renamed to a file that already exists
	// but that is not part of the files to rename...
	if (CPath::PathFileExists(strAfterLower)	// The destination exists on the disk.
		&& setBeforeLower.find(strAfterLower) == setBeforeLower.end())	// and it's not going to be renamed.
	{
		// No it is not, so it will conflict with the existing file on the disk.
		CString strErrorMsg;
		strErrorMsg.LoadString(IDS_CONFLICT_WITH_EXISTING);
		SetProblem(nOperationIndex, errConflict, strErrorMsg);
	}
	// If it's not going to conflict with a file not part of the files to rename,
	// check if it conflicts with files that are going to be renamed...
	else
	{
		// Check if two files are going to have the same new file name.
		map<CString, int>::const_iterator iterFound = mapAfterLower.find(strAfterLower);
		if (iterFound != mapAfterLower.end())
		{
			// Conflict found: Two files are going to be renamed to the same new file name.
			CString strErrorMsg;

			AfxFormatString1(strErrorMsg, IDS_CONFLICT_SAME_AFTER, m_vRenamingOperations[iterFound->second].pathBefore.GetPath());
			SetProblem(nOperationIndex, errConflict, strErrorMsg);

			AfxFormatString1(strErrorMsg, IDS_CONFLICT_SAME_AFTER, m_vRenamingOperations[nOperationIndex].pathBefore.GetPath());
			SetProblem(iterFound->second, errConflict, strErrorMsg);
		}
		else
		{// We add this file to the map.
			mapAfterLower.insert( pair<CString, int>(strAfterLower, nOperationIndex) );
		}
	}
}

void CRenamingList::CheckDirectoryPath(int nOperationIndex)
{
	const CPath* pOperationPathAfter = &m_vRenamingOperations[nOperationIndex].pathAfter;

	// Check that the directory starts and ends by a backslash.
	const CString& strRoot = pOperationPathAfter->GetPathRoot();
	const CString& strDir = pOperationPathAfter->GetDirectoryName();

	if (strRoot.IsEmpty() || 
		strRoot[strRoot.GetLength() - 1] != '\\' ||
		(!strDir.IsEmpty() && strDir[strDir.GetLength() - 1] != '\\'))
	{
		CString strErrorMsg;
		strErrorMsg.LoadString(IDS_START_END_BACKSLASH_MISSING);
		SetProblem(nOperationIndex, errBackslashMissing, strErrorMsg);
	}

	// Check if the directories names are valid.
	BOOST_FOREACH(CString strDirectory, pOperationPathAfter->GetDirectories())
	{
		// Find the directory's name before the '.' (yes an extension is also counted for directories)
		CString strDirectoryWithoutExtension;
		int nPos = strDirectory.Find('.');
		if (nPos != -1)
			strDirectoryWithoutExtension = strDirectory.Left(nPos);
		else
			strDirectoryWithoutExtension = strDirectory;

		// Check the name.
		COperationProblem problem = CheckName(strDirectory, strDirectoryWithoutExtension, false);
		if (problem.nErrorLevel != levelNone)
		{
			// Some problem found.
			SetProblem(nOperationIndex, problem.nErrorCode, problem.strMessage);
			break;
		}
	} // end: directory name validity check.

	// Check if the path if over MAX_PATH.
	if (CPath::MakeSimplePath(pOperationPathAfter->GetPath()).GetLength() >= MAX_PATH)
	{
		CString strErrorMsg;
		strErrorMsg.LoadString(IDS_LONGUER_THAN_MAX_PATH);
		SetProblem(nOperationIndex, errLonguerThanMaxPath, strErrorMsg);
	}
}

CRenamingList::COperationProblem CRenamingList::CheckName(const CString& strName, const CString& strNameWithoutExtension, bool bIsFileName)
{
	// Empty name.
	if (strName.IsEmpty())
	{
		// Warning: Invalid.
		COperationProblem problem;
		if (bIsFileName)
		{
			problem.nErrorLevel = levelError;
			problem.nErrorCode = errInvalidFileName;
			problem.strMessage.LoadString(IDS_EMPTY_FILE_NAME);
		}
		else
		{
			problem.nErrorLevel = levelError;
			problem.nErrorCode = errInvalidDirectoryName;
			problem.strMessage.LoadString(IDS_EMPTY_DIRECTORY_NAME);
		}
		return problem;
	}

	// Invalid path characters might include ASCII/Unicode characters 1 through 31, as well as quote ("), less than (<), greater than (>), pipe (|), backspace (\b), null (\0) and tab (\t).
	if (strName.FindOneOf(_T("\\/:*?\"<>|\b\t")) != -1	// Forbidden characters.
		|| strName.Right(0) == _T("."))	// The OS doesn't support files/directories ending by a dot.
	{
		// Warning: Invalid.
		COperationProblem problem;
		if (bIsFileName)
		{
			problem.nErrorLevel = levelError;
			problem.nErrorCode = errInvalidFileName;
			problem.strMessage.LoadString(IDS_INVALID_FILE_NAME);
		}
		else
		{
			problem.nErrorLevel = levelError;
			problem.nErrorCode = errInvalidDirectoryName;
			problem.strMessage.LoadString(IDS_INVALID_DIRECTORY_NAME);
		}
		return problem;
	}

	// Look for some other characters that are invalid: 0x00 to 0x20.
	for (int i=0; i<strName.GetLength(); ++i)
	{
		// ASCII/Unicode characters 1 through 31, as well as null (\0), are not allowed.
		if (strName[i] < 0x20)
		{
			// Windows XP rejects any file name with a character below 32.
			COperationProblem problem;
			if (bIsFileName)
			{
				problem.nErrorLevel = levelError;
				problem.nErrorCode = errInvalidFileName;
				problem.strMessage.LoadString(IDS_INVALID_FILE_NAME);
			}
			else
			{
				problem.nErrorLevel = levelError;
				problem.nErrorCode = errInvalidDirectoryName;
				problem.strMessage.LoadString(IDS_INVALID_DIRECTORY_NAME);
			}
			return problem;
		}
	}

	// Check if it's a name to avoid.

	// Over 120 characters (maximum allowed from the Explorer)
	if (strName.GetLength() > 120)
	{
		// Warning: Too long.
		if (bIsFileName)
		{
			COperationProblem problem;
			problem.nErrorLevel = levelWarning;
			problem.nErrorCode = errRiskyFileName;
			problem.strMessage.LoadString(IDS_RISKY_FNAME_TOO_LONG);
			return problem;
		}
		else
		{
			COperationProblem problem;
			problem.nErrorLevel = levelWarning;
			problem.nErrorCode = errRiskyDirectoryName;
			problem.strMessage.LoadString(IDS_RISKY_DIRNAME_TOO_LONG);
			return problem;
		}
	}

	// The following reserved words cannot be used as the name of a file:
	// CON, PRN, AUX, CLOCK$, NUL, COM1, COM2, ..., COM9, LPT1, LPT2, ..., LPT9.
	// Also, reserved words followed by an extension�for example, NUL.tx7�are invalid file names.
	const int MAX_INVALID_NAME_LENGTH = 6; // = strlen("CLOCK$");
	if ((strNameWithoutExtension.GetLength() <= MAX_INVALID_NAME_LENGTH	// Quickly remove any file name that can't be invalid...
			&& (   strNameWithoutExtension.CompareNoCase(_T("CON")) == 0	// to quickly detect system reserved file names.
				|| strNameWithoutExtension.CompareNoCase(_T("PRN")) == 0
				|| strNameWithoutExtension.CompareNoCase(_T("AUX")) == 0
				|| strNameWithoutExtension.CompareNoCase(_T("CLOCK$")) == 0 
				|| strNameWithoutExtension.CompareNoCase(_T("NUL")) == 0 
				|| (strNameWithoutExtension.GetLength() == 4
					&& (_tcsncicmp(strNameWithoutExtension, _T("COM"), 3) == 0 || _tcsncicmp(strNameWithoutExtension, _T("LPT"), 3) == 0)
					&& (strNameWithoutExtension[3] >= _T('1') && strNameWithoutExtension[3] <= _T('9'))
				   )
				)
		   )
	   )
	{
		// Warning: Reserved file name.
		COperationProblem problem;
		if (bIsFileName)
		{
			problem.nErrorLevel = levelWarning;
			problem.nErrorCode = errRiskyFileName;
			problem.strMessage.LoadString(IDS_RISKY_FNAME_RESERVED);
		}
		else
		{
			problem.nErrorLevel = levelWarning;
			problem.nErrorCode = errRiskyDirectoryName;
			problem.strMessage.LoadString(IDS_RISKY_DIRNAME_RESERVED);
		}
		return problem;
	}

	// Starting or ending by one or more spaces.
	ASSERT(!strName.IsEmpty());
	if (strName[0] == _T(' ') ||						// Files/Directories starting by a space is not good.
		strName[strName.GetLength() - 1] == _T(' '))	// Files/Directories ending by a space is not good.
	{
		// Warning: Spaces before or after.
		COperationProblem problem;
		if (bIsFileName)
		{
			problem.nErrorLevel = levelWarning;
			problem.nErrorCode = errRiskyFileName;
			problem.strMessage.LoadString(IDS_RISKY_FNAME_TRIM_SPACES);
		}
		else
		{
			problem.nErrorLevel = levelWarning;
			problem.nErrorCode = errRiskyDirectoryName;
			problem.strMessage.LoadString(IDS_RISKY_DIRNAME_TRIM_SPACES);
		}
		return problem;
	}

	return COperationProblem();
}

void CRenamingList::SetProblem(int nOperationIndex, EErrorCode nErrorCode, CString strMessage)
{
	COperationProblem& operationProblem = m_vProblems[nOperationIndex];

	// Errors should always go up and keep the highest one only.
	if (nErrorCode > operationProblem.nErrorCode)
	{
		// Find the error level from the error code.
		EErrorLevel nLevel;
		BOOST_STATIC_ASSERT(errCount == 10);
		switch (nErrorCode)
		{
		case errDirCaseInconsistent:
		case errLonguerThanMaxPath:
		case errRiskyFileName:
		case errRiskyDirectoryName:
			nLevel = levelWarning;
			break;

		default:
			nLevel = levelError;
			break;
		}

		// Update error counters.
		switch (nLevel)
		{
		case levelWarning:	++m_nWarnings; break;
		case levelError:	++m_nErrors; break;
		}

		// Save the problem in the report.
		operationProblem.nErrorLevel = nLevel;
		operationProblem.nErrorCode = nErrorCode;
		operationProblem.strMessage = strMessage;
		ASSERT((operationProblem.nErrorLevel==levelNone) ^ (operationProblem.nErrorCode!=errNoError)); // no error <=> no error code, an error <=> error code set
		ASSERT((operationProblem.nErrorLevel==levelNone) ^ !operationProblem.strMessage.IsEmpty());	// no error <=> no error message, an error <=> error message set
	}
}

bool CRenamingList::PerformRenaming(CKtmTransaction& ktm)
{
	// Avoid possible strange behaviors for empty lists.
	if (m_vRenamingOperations.size() == 0)
		return true;

	// Pre-conditions checking.
#ifdef _DEBUG
	BOOST_FOREACH(CRenamingOperation& operation, m_vRenamingOperations)
	{
		// Each operation must be a Unicode path.
		ASSERT(operation.pathBefore.GetPath().Left(4) == "\\\\?\\");
		ASSERT(operation.pathAfter.GetPath().Left(4) == "\\\\?\\");

		// Each operation changes something.
		ASSERT(operation.pathBefore != operation.pathAfter);	// Note that this is case sensitive.
	}
#endif

	// Transform the set of renaming operations into an ordered list of
	// operations arranged so that by performing all the operations
	// one after another is possible and will result in performing all the
	// renaming operations asked.
	// Note: This may modify the m_vRenamingOperations[].
	vector<shared_ptr<CIOOperation>> vOperationList = PrepareRenaming();

	// Failover to non-KTM when ::GetLastError() == ERROR_RM_NOT_ACTIVE
	// TODO: See how to alert the user or even tell him before
	//       renaming that KTM will or will not be supported.
	//       Could use the FindFile to detect before renaming.
	// FIXME: When KTM is supported on the system but not on some of its
	//        file systems (like the case here), the error report dialog
	//        still proposes to roll back even though it can't.

	// Rename files order.
	OnProgress(stageRenaming, 0, (int)vOperationList.size());	// Inform we start renaming.

	bool bError = false;
	for (unsigned i=0; i<vOperationList.size(); ++i)
	{
		// Perform the operation.
		CIOOperation::EErrorLevel nErrorLevel = vOperationList[i]->Perform(ktm);
		if (nErrorLevel != CIOOperation::elSuccess)
		{
			OnRenameError(*vOperationList[i], nErrorLevel);
			bError = true;
		}

		// Report progress.
		OnProgress(stageRenaming, i, (int)vOperationList.size());	// Inform we start renaming.
	}

	return !bError;
}

void CRenamingList::OnRenamed(int nIndex)
{
	if (m_fOnRenamed)
		m_fOnRenamed(
			m_vRenamingOperations[nIndex].GetPathBefore(),
			m_vRenamingOperations[nIndex].GetPathAfter());
}

void CRenamingList::OnRenameError(const CIOOperation& ioOperation, CIOOperation::EErrorLevel nErrorLevel)
{
	if (m_fOnRenameError)
		m_fOnRenameError(ioOperation, nErrorLevel);
}

void CRenamingList::OnProgress(EStage nStage, int nDone, int nTotal)
{
	if (m_fOnProgress)
		m_fOnProgress(nStage, nDone, nTotal);
}

vector<shared_ptr<CIOOperation>> CRenamingList::PrepareRenaming()
{
	///////////////////////////////////////////////////////////////////
	// Create an oriented graph of conflicting renaming operations
	Beroux::Math::OrientedGraph	graph;
	const int nFilesCount = (int) m_vRenamingOperations.size();

	// Change the locale to match the file system stricmp().
	CScopedLocale scopeLocale(_T(""));

	// Create a first version of an oriented graph of conflicting renaming operations:
	// Node(i) -edge-to-> Node(j)   <=>   File(i).originalFileName == File(j).newFileName (compare no case)
	//                              <=>   File(i) must be renamed before File(j)
	map<CString, int> mapAfterLower;
	
	// Create a node for each renaming operation,
	// and the mapAfterLower.
	for (int i=0; i<nFilesCount; ++i)	
	{
		// Report progress
		OnProgress(stagePreRenaming, i*20/nFilesCount, 100);

		graph.AddNode(i);

		CString strAfter = m_vRenamingOperations[i].pathAfter.GetPath();
		mapAfterLower.insert( pair<CString, int>(strAfter.MakeLower(), i) );
	}

	// Unify folders' case (1/2): Find the length of the shortest path.
	int nMinAfterDirIndex = FindShortestDirectoryPathAfter(m_vRenamingOperations);

	map<CString, DIR_CASE, dir_case_compare> mapDirsCase;
	// Insert the shortest path to the map (to improve a little the speed).
	{
		CString strShortestDirAfter = m_vRenamingOperations[nMinAfterDirIndex].pathAfter.GetDirectoryName();
		mapDirsCase.insert( pair<CString, DIR_CASE>(
			strShortestDirAfter.Left(strShortestDirAfter.GetLength() - 1),
			DIR_CASE(nMinAfterDirIndex, strShortestDirAfter.GetLength())) );
	}

	// Directories to remove if empty (ordered set by longest path first).
	set<CString, path_compare<CString> > setDeleteIfEmptyDirectories;

	// Define the successors in the graph,
	// and add folders to later erase if they're empty.
	for (int i=0; i<nFilesCount; ++i)
	{
		// Report progress
		OnProgress(stagePreRenaming, 20 + i*75/nFilesCount, 100);

		// Definitions
		CPath* proBefore = &m_vRenamingOperations[i].pathBefore;
		CPath* proAfter = &m_vRenamingOperations[i].pathAfter;

		// Look if there is a node with name-after equal to this node's name-before
		// (meaning that there should be an edge from the this node to the found node).
		// We ignore when the operation `i` has the same name a before and after
		// (since the checking would then be handled by the file system).
		CString strBefore = proBefore->GetPath();
		map<CString, int>::const_iterator iterFound = mapAfterLower.find(strBefore.MakeLower());
		if (iterFound != mapAfterLower.end() && iterFound->second != i)
		{
			// Node(i) -edge-to-> Node(j)
			graph[i].AddSuccessor(iterFound->second);
			
			// Assertion: There can be at most one successor,
			// else it means that two files (or more) will have the same destination name.

			// Check if their are part of a cycle (only nodes that have a successor and an antecedent may form a cycle).
			if (graph[i].HasSuccessor() && graph[i].HasAntecedent())
			{
				for (int j=graph[i].GetSuccessor(0); graph[j].HasSuccessor(); j=graph[j].GetSuccessor(0))
					if (i == j)	// Cycle found: Node(i) --> Node(i).successor --> ... --> Node(i).
					{
						// Add a new node, to make a temporary rename that solves the problem.
						// It is fixes the problem because the temporary file doesn't exist yet,
						// so it doesn't conflict.

						// Originally rename A -> B
						int nOperationIndex = graph[i].GetSuccessor(0);
						CPath pathFinal = m_vRenamingOperations[nOperationIndex].pathAfter;

						// Find a temporary name to rename A -> TMP -> B.
						CString strRandomName;
						{
							ASSERT(pathFinal.GetPath()[pathFinal.GetPath().GetLength() - 1] != '\\');
							strRandomName = pathFinal.GetPath() + _T("~TMP");

							CString strRandomNameFormat = strRandomName + _T("%d");
							for (int k=2; CPath::PathFileExists(strRandomName) || mapAfterLower.find(strRandomName) != mapAfterLower.end(); ++k)
								strRandomName.Format(strRandomNameFormat, k);
						}
						CPath pathTemp = strRandomName;

						// Rename A -> TMP
						m_vRenamingOperations[nOperationIndex].pathAfter = pathTemp;
						// (Optional operation since mapAfterLower.find(X.before) == This operation.)
						//CString strAfter = pathTemp.GetPath();
						//mapAfterLower.insert( pair<CString, int>(strAfter.MakeLower(), nOperationIndex) );

						// Then rename TMP -> B
						AddRenamingOperation( CRenamingOperation(pathTemp, pathFinal) );
						int nSecondOperationIndex = (int)m_vRenamingOperations.size() - 1;
						CString strAfter = pathTemp.GetPath();
						mapAfterLower[strAfter.MakeLower()] = nSecondOperationIndex;

						// Since we changed the vector array, the memory location may have been changed also.
						proBefore = &m_vRenamingOperations[i].pathBefore;
						proAfter = &m_vRenamingOperations[i].pathAfter;

						// Add Node(i) --> Node(TMP), because File(i) must be renamed before File(TMP).
						graph.AddNode(nSecondOperationIndex);
						graph[i].RemoveSuccessor(0);
						graph[i].AddSuccessor(nSecondOperationIndex);

						break;
					}
			} // end if cycle detection.
		} // end if successor found.

		// Add directories to remove later if they're empty.
		if (proBefore->GetDirectoryName().CompareNoCase(proAfter->GetDirectoryName()) != 0 &&
			!DirectoryIsEmpty(proBefore->GetDirectoryName()))
		{
			CString strParentPath = proBefore->GetPathRoot();
			BOOST_FOREACH(CString strDirectoryName, proBefore->GetDirectories())
			{
				// Get the full parent directory's path.
				strParentPath += strDirectoryName.MakeLower();
				strParentPath.AppendChar('\\');

				// Add to the set.
				setDeleteIfEmptyDirectories.insert(strParentPath);
			}
		}

		// Unify folders' case 2/2
		{
			// Create a copy of the folder name.
			CString strDirAfter = proAfter->GetDirectoryName();
			ASSERT(strDirAfter[strDirAfter.GetLength() - 1] == '\\');
			strDirAfter = strDirAfter.Left(strDirAfter.GetLength() - 1);

			// For each parent directory name (starting from GetDirectoryName()).
			const int nMinDirAfterLength = m_vRenamingOperations[nMinAfterDirIndex].pathAfter.GetDirectoryName().GetLength() - 1; // note we don't count the last '\' since we remove it.
			while (strDirAfter.GetLength() >= nMinDirAfterLength)
			{
				// If the directory is in the map.
				map<CString, DIR_CASE, dir_case_compare>::iterator iter = mapDirsCase.find(strDirAfter);
				if (iter == mapDirsCase.end())
				{
					// Add it to the map.
					mapDirsCase.insert( pair<CString, DIR_CASE>(
						strDirAfter,
						DIR_CASE(i, strDirAfter.GetLength())) );
				}
				else
				{
					// Check if the case differ.
					if (iter->first != strDirAfter)
					{
						// If the RO's length > map's length,
						// then it means that that map's case should prevail.
						if (strDirAfter.GetLength() >= iter->second.nMinDirLength)
						{
							// Change the RO's case.
							*proAfter = iter->first + proAfter->GetPath().Mid(iter->first.GetLength());
						}
						else
						{
							// Change the map's case and update the map's min length.
							// Since changing the case doesn't affect this operation's order in the map, we can const_cast<>.
							const_cast<CString&>(iter->first) = strDirAfter;
							iter->second.nMinDirLength = strDirAfter.GetLength();

							// Change the map's case, all RO's in the map.
							BOOST_FOREACH(int nROIndex, iter->second.vnOperationsIndex)
							{
								m_vRenamingOperations[nROIndex].pathAfter = strDirAfter + m_vRenamingOperations[nROIndex].pathAfter.GetPath().Mid(strDirAfter.GetLength());
							}
						}
					} // end: Check if the case differ.

					// Add the RO's index.
					iter->second.vnOperationsIndex.push_back( i );
				} // end if the directory is in the map.

				// Go to the next parent folder.
				int nPos = strDirAfter.ReverseFind('\\');
				if (nPos == -1)
					break;
				strDirAfter = strDirAfter.Left(nPos);
			} // end while.
		} // end of case unification.
	} // end for each operations index `i`.

	///////////////////////////////////////////////////////////////////
	// Finally, create an ordered map of elements to rename so that the
	// longest path comes first.
	map<CString, int, path_compare<CString> > mapRenamingOperations;
	typedef std::pair<CString, int> ro_pair_t;
	{
		const int nTotal = (int) m_vRenamingOperations.size();
		for (int i=0; i<nTotal; ++i)
		{
			// Report progress
			OnProgress(stagePreRenaming, 95 + i*5/nTotal, 100);

			if (!graph[i].HasAntecedent())
				mapRenamingOperations.insert( ro_pair_t(m_vRenamingOperations[i].pathBefore.GetPath(), i) );
		}
	}

	// Create a list of operations index that'll provide the files to rename
	// in topological order.
	vector<int> vOrderedOperationsIndexes;
	BOOST_FOREACH(ro_pair_t pair, mapRenamingOperations)
	{
		int nIndex = pair.second;
		while (true)
		{
			vOrderedOperationsIndexes.push_back(nIndex);

			// Rename its successors.
			if (graph[nIndex].HasSuccessor())
				nIndex = graph[nIndex].GetSuccessor(0);
			else
				break;
		}
	}

	// Create the list of operations
	vector<shared_ptr<CIOOperation>> vOrderedOperationList;

	// Add renaming operation that would change the case of existing
	// folders if different from the existing one (part of the folder case unification process).
	{
		for (map<CString, DIR_CASE, dir_case_compare>::const_iterator iter = mapDirsCase.begin(); iter != mapDirsCase.end(); ++iter)
		{
			// If the folder currently exists with another case,
			if (CPath::PathFileExists(iter->first))
			{
				CString strCurrentPathName = CPath::FindPathCase(iter->first);
				if (strCurrentPathName != iter->first)
				{
					vOrderedOperationList.push_back(shared_ptr<CIOOperation>(
						new CRenameOperation(strCurrentPathName, iter->first)
					));
				}
			}
		}
 	}

	// Add requested renaming operations.
	BOOST_FOREACH(unsigned nIndex, vOrderedOperationsIndexes)
	{
		const CRenamingOperation& renamingOperation = m_vRenamingOperations[nIndex];

		// Check that every parent directory exists, or create it.
		vOrderedOperationList.push_back(shared_ptr<CIOOperation>(
			new CCreateDirectoryOperation(renamingOperation.pathAfter)
		));

		// Rename file.
		vOrderedOperationList.push_back(shared_ptr<CIOOperation>(
			new CRenameOperation(renamingOperation.pathBefore.GetPath(), renamingOperation.pathAfter.GetPath())
		));
	}

	// Delete emptied folders (folders that are empty after renaming).
	// Note that the set is ordered so that the longest path comes first,
	// which implies that sub-folders are removed prior to checking their parent folder.
	// Mark folders that should be erased after renaming if they are empty.
	BOOST_FOREACH(CString& strDirectoryPath, setDeleteIfEmptyDirectories)
	{
		// Remove ONLY a non-empty directory.
		vOrderedOperationList.push_back(shared_ptr<CIOOperation>(
			new CRemoveEmptyDirectoryOperation(strDirectoryPath)
		));
	}

	return vOrderedOperationList;
}

int CRenamingList::FindShortestDirectoryPathAfter(vector<CRenamingOperation>& vRenamingOperations)
{
	int nMinAfterDirIndex = 0;
	int nMinAfterDirLength = vRenamingOperations[nMinAfterDirIndex].pathAfter.GetDirectoryName().GetLength();

	// Find the shortest path.
	const int nFilesCount = (int)vRenamingOperations.size();
	for (int i=0; i<nFilesCount; ++i)
	{
		int nDirLength = vRenamingOperations[i].pathAfter.GetDirectoryName().GetLength();

		if (nDirLength < nMinAfterDirLength)
		{
			nMinAfterDirIndex = i;
			nMinAfterDirLength = nDirLength;
		}
	}

	return nMinAfterDirIndex;
}

bool CRenamingList::DirectoryIsEmpty(const CString& strDirectoryPath, CKtmTransaction* pKTM /*= NULL*/)
{
	ASSERT(strDirectoryPath.Right(1) == '\\');

	WIN32_FIND_DATA fd;
	HANDLE hFindFile;

	if (pKTM != NULL)
		hFindFile = pKTM->FindFirstFileEx(strDirectoryPath + _T("*.*"), FindExInfoStandard, &fd, FindExSearchNameMatch, NULL, 0);
	else
		hFindFile = FindFirstFileEx(strDirectoryPath + _T("*.*"), FindExInfoStandard, &fd, FindExSearchNameMatch, NULL, 0);

	if (hFindFile == INVALID_HANDLE_VALUE)
	{
		DWORD dwErrorCode = ::GetLastError();
#ifdef _DEBUG
		// Get error message
		LPTSTR lpMsgBuf = NULL;
		FormatMessage( 
			FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
			NULL,
			dwErrorCode,
			MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), // Default language
			(LPTSTR) &lpMsgBuf,
			0,
			NULL );
		CString strErrorMessage = lpMsgBuf;
		LocalFree( lpMsgBuf );
#endif
		ASSERT(dwErrorCode == ERROR_PATH_NOT_FOUND);
		return false;
	}
	else
	{
		do
		{
			if (_tcscmp(fd.cFileName, _T(".")) != 0 &&
				_tcscmp(fd.cFileName, _T("..")) != 0)
			{
				::FindClose(hFindFile);
				return false;
			}
		} while (::FindNextFile(hFindFile, &fd));

		::FindClose(hFindFile);
		return true;
	}
}

}}}
