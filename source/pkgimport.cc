
#define LOCAL_DEBUG
#include "debug.h"

#include "pkgimport.h"
#include "header.h"
#include "dirwalk.h"
#include "meta.h"
#include "acfg.h"
#include "filereader.h"
#include "dlcon.h"
#include "csmapping.h"

#include <string>
#include <iostream>
#include <fstream>
#include <meta.h>
#include <cstdio>
#include <errno.h>

using namespace MYSTD;

#define SCACHEFILE (CACHE_BASE+"_impkeycache")
#define FMTSIG "FMT5"

/*
 * Algorithm:
 *
 * Scan the _import directory context and make a dictionary with
 * fingerprint(size, cs) -> (path, bUsed, mtime)
 * in the ProcessRegular callback function
 *
 * Parse the vol. files, do work, and mark used file with bUsed
 *
*/

pkgimport::pkgimport(int fd) : tCacheMan(fd), m_bLookForIFiles(false)
{
	m_sTypeName="File Import";
}

/*
inline bool IsIndexDiff(const string & sPath)
{
	static tFingerprint fprBuf;
	static string sDiffHint(".diff" SZPATHSEP);
	tStrPos pos;
	return (endsWithSzAr(sPath,".gz") && (sPath.find(sDiffHint) != stmiss || (stmiss != (pos
			= sPath.rfind(SZPATHSEP "20")) && sPath.find_first_not_of("0123456789.-", pos + 3)
			== sPath.size() - 2)));
}
*/

bool pkgimport::ProcessRegular(cmstring &sPath, const struct stat &stinfo)
{

	{
		lockguard g(&abortMx);
		if(bSigTaskAbort)
			return false;
	}
	if(endsWithSzAr(sPath, ".head"))
		return true;

	if(sPath.size()<=CACHE_BASE_LEN) // heh?
		return false;
	
	if(m_bLookForIFiles)
	{
		ProgTell();

		if(0==sPath.compare(CACHE_BASE_LEN, 1, "_"))
			return true; // not for us, also excludes _import

		AddIFileCandidate(sPath.substr(CACHE_BASE_LEN));
	}
	else if(rechecks::FILE_INVALID != rechecks::GetFiletype(sPath))
	{
		// get a fingerprint by checksumming if not already there from the fpr cache

		if(m_precachedList.find(sPath)!=m_precachedList.end())
			return true; // have that already, somewhere...
		
		for (CSTYPES ctp = CSTYPE_MD5; ctp <= CSTYPE_SHA1; ctp=CSTYPES(int(ctp)+1))
		{

			// get the most likely requested contents id
			tFingerprint fpr;
			/*	if ( (IsIndexDiff(sPath) && fpr.ScanFile(sPath, CSTYPE_SHA1, true))
			 || (!IsIndexDiff(sPath) && fpr.ScanFile(sPath, CSTYPE_MD5, false)))
			 */
			if (!fpr.ScanFile(sPath, ctp, false, NULL))
			{
				SendFmt << "<span class=\"ERROR\">Error checking " << sPath << "</span>\n<br>\n";
				continue;
			}
			if (fpr.size < 50)
			{
				// ultra small files, looking like garbage (gz'ed empfy file, ...)
				continue;
			}

			SendFmt << "<font color=blue>Checked " << sPath << " (" << (ctp == CSTYPE_MD5
					? "MD5"
					: "SHA1") << " fingerprint created)</font><br>\n";

			// add this entry immediately if needed and get its reference
			tImpFileInfo & node = m_importMap[fpr];
			if (node.sPath.empty())
			{ // fresh, just added to the map
				node.sPath = sPath;
				node.mtime = stinfo.st_mtime;
			}
			else if (node.sPath != sPath)
			{
				SendFmt << "<span class=\"WARNING\">Duplicate found, " << sPath << " vs. "
						<< node.sPath << ", ignoring new entry.</span>\n<br>\n";
				m_importRest.push_back(make_pair(fpr, tImpFileInfo(sPath, stinfo.st_mtime)));
			}
		}
	}
	else
	{
		if(m_bVerbose)
			SendFmt << "<span class=\"WARNING\">File type unknown, skipping "+sPath << "</span>\n<br>\n";
	}
	return true;
}

bool LinkOrCopy(const MYSTD::string &from, const MYSTD::string &to)
{
	mkbasedir(to);
	
	//ldbg("Moving " <<from<< " to " << to);
	
	// hardlinks don't work with symlinks properly, create a similar symlink then
	struct stat stinfo, toinfo;
	
	if(0==lstat(from.c_str(), &stinfo) && S_ISLNK(stinfo.st_mode))
	{
		char buf[PATH_MAX+1];
		buf[PATH_MAX]=0x0;
		
		if (!realpath(from.c_str(), buf))
			return false;

		::unlink(to.c_str());
		return (0==symlink(buf, to.c_str()));		
	}
	else
	{
		if(0!=stat(from.c_str(), &stinfo))
				return false;
		
		if(0==stat(to.c_str(), &toinfo))
		{
			// same file, don't care
			if(stinfo.st_dev==toinfo.st_dev && stinfo.st_ino == toinfo.st_ino)
				return true;
			
			// unlink, this file must go. If that fails, we have weird permissions anyway.
			if(0!=unlink(to.c_str()))
				return false;
		}
		if (0==link(from.c_str(), to.c_str()))
			return true;

		if(!FileCopy(from,to))
			return false;
		
		// make sure the copy is ok
		return (0 == stat(from.c_str(), &stinfo) 
				&& 0 == stat(to.c_str(), &toinfo)
				&& toinfo.st_size==stinfo.st_size);
		
	}

	return false;
}

			
void pkgimport::Action(const string &cmd) 
{
// TODO: import path from cmd?
	m_sSrcPath=CACHE_BASE+"_import";
	
	SendFmt << "Importing from " << m_sSrcPath << " directory.<br>Scanning local files...<br>";
	
	SetCommonUserFlags(cmd);
	m_bErrAbort=false; // does not f...ing matter, do what we can
	m_bByPath=true; // should act on all locations

	_LoadKeyCache(SCACHEFILE);
	if(!m_precachedList.empty())
		SendChunk( tSS(100)<<"Loaded "<<m_importMap.size()<<
				(m_precachedList.size()==1 ? " entry" : " entries")
				<<" from the fingerprint cache<br>\n");
	
	m_bLookForIFiles=true;
	DirectoryWalk(acfg::cachedir, this);

	{
		lockguard g(&abortMx);
		if(bSigTaskAbort)
			return;
	}
	
	if(m_indexFilesRel.empty())
	{
		SendChunk("<span class=\"ERROR\">No index files detected. Unable to continue, cannot map files to internal locations.</span>");
		return;
	}


	UpdateIndexFiles();

	{
		lockguard g(&abortMx);
		if(bSigTaskAbort)
			return;
	}
	
	if(m_bErrAbort && m_nErrorCount>0)
	{
		SendChunk("Found errors during processing, aborting as requested.");
		return;
	}
	
	m_bLookForIFiles=false;
	DirectoryWalk(m_sSrcPath, this);

	{
		lockguard g(&abortMx);
		if(bSigTaskAbort)
			return;
	}
	
	if(m_importMap.empty())
	{
		SendChunk("No appropriate files found in the _import directory.<br>\nDone.<br>\n");
		return;
	}
	
	ProcessSeenIndexFiles(*this);

	{
		lockguard g(&abortMx);
		if(bSigTaskAbort)
			return;
	}

	ofstream fList;
	fList.open(SCACHEFILE.c_str(), ios::out | ios::trunc);

	if(!fList.is_open())
	{
		unlink(SCACHEFILE.c_str());
		fList.open(SCACHEFILE.c_str(), ios::out| ios::trunc);
	}
	if(!fList.is_open())
		SendChunk("Cannot save fingerprint cache, ignored");
	else
		fList << FMTSIG"\n";
	
	off_t remaining=0;

	for(tImportMap::const_iterator it=m_importMap.begin();
	it!=m_importMap.end();it++)
	{
		// delete imported stuff, and cache the fingerprint only when file was deleted
		
		if(it->second.bFileUsed && 0==unlink(it->second.sPath.c_str()))
			continue;
		if(!fList.is_open())
			continue;

#define ENDL "\n" // don't flush all the time
		fList << it->first.size << ENDL
				<< (int)it->first.csType << ENDL
				<< it->first.GetCsAsString() << ENDL
				<< it->second.sPath.substr(m_sSrcPath.size()+1) <<ENDL
				<< it->second.mtime	<<ENDL;

		remaining+=it->first.size;
	}

	// deal with the rest, never double-examine that stuff
	// copy&paste FTW
	for(tImportList::const_iterator it=m_importRest.begin();
	it!=m_importRest.end();it++)
	{
		fList << it->first.size << ENDL
				<< (int)it->first.csType << ENDL
				<< it->first.GetCsAsString() << ENDL
				<< it->second.sPath.substr(m_sSrcPath.size()+1) <<ENDL
				<< it->second.mtime	<<ENDL;
		remaining+=it->first.size;
	}
	fList.flush();
	fList.close();

	PrintStats("Imported data, per target");

	int n=(m_importMap.size()+m_importRest.size());
	if(n)
		SendFmt << "\n<br>" << n << (n==1 ? " file (" : " files (")
		<< offttosH(remaining) << ") left behind";
}

void pkgimport::HandlePkgEntry(const tRemoteFileInfo &entry, bool)
{
	//typedef MYMAP<tFingerprint, tImpFileInfo, ltfingerprint> tImportMap;
	tImportMap::iterator hit = m_importMap.find(entry.fpr);
	if (hit==m_importMap.end())
		return;
	
	string sDestAbs=CACHE_BASE;
	if(acfg::stupidfs)
		sDestAbs+=DosEscape(entry.sDirectory+entry.sFileName);
	else
		sDestAbs+=(entry.sDirectory+entry.sFileName);

	string sDestHeadAbs=sDestAbs+".head";
	const string &sFromAbs=hit->second.sPath;

	SendChunk(string("<font color=green>HIT: ")+sFromAbs
			+ "<br>\nDESTINATION: "+sDestAbs+"</font><br>\n");

	// linking and moving would shred them when the link leads to the same target
	struct stat tmp1, tmp2;

	if (0==stat(sDestAbs.c_str(), &tmp1) && 0==stat(sFromAbs.c_str(), &tmp2)
			&& tmp1.st_ino==tmp2.st_ino&& tmp1.st_dev==tmp2.st_dev)
	{
		//cerr << "Same target file, ignoring."<<endl;
		hit->second.bFileUsed=true;
		SendChunk("<span class=\"WARNING\">Same file exists</span><br>\n");
		if(0!=access(sDestHeadAbs.c_str(), F_OK))
		{
			SendChunk("<span class=\"WARNING\">Header is missing, will recreate...</span>\n<br>\n");
			goto gen_header;
		}
		return;
	}

	unlink(sDestAbs.c_str());

	// XXX: maybe use inject instead

	if (!LinkOrCopy(sFromAbs, sDestAbs))
	{
		SendChunk("<span class=\"ERROR\">ERROR: couldn't link or copy file.</span>\n<br>\n");
		return;
	}

	gen_header:
	unlink(sDestHeadAbs.c_str());

	header h;
	h.frontLine="HTTP/1.1 200 OK";
	/*
	if(entry.fpr.bUnpack)
	{
		static struct stat stbuf;
		if(0!=stat(sFrom.c_str(), &stbuf))
			return; // weird...
		h.set(header::CONTENT_LENGTH, stbuf.st_size);
	}
	else
	*/
		h.set(header::CONTENT_LENGTH, entry.fpr.size);
	
	h.type=header::ANSWER;
	if (h.StoreToFile(sDestAbs+".head")<=0)
	{
		aclog::err("Unable to store generated header");
		return; // junk may remain but that's a job for cleanup later
	}
	hit->second.bFileUsed=true;

	SetFlags(m_processedIfile).space+=entry.fpr.size;
}


void pkgimport::_LoadKeyCache(const string & sFileName)
{
	MYSTD::ifstream in;

	tImpFileInfo info;
	tFingerprint fpr;
	
	in.open(SCACHEFILE.c_str());
	string cs;
	if(!in.is_open())
		return;

	MYSTD::getline(in, cs);
	if(cs!=FMTSIG)
		return;

/*	if(m_bVerbose)
		SendChunk("Loading fingerprints from key cache... \n");
		*/
	off_t sz(-1);
	int csType(CSTYPE_INVALID);

	for(;;)
	{
		info.bFileUsed=false;

		in>>sz;
		MYSTD::getline(in, cs); // newline

		in>>csType;
		MYSTD::getline(in, cs); // newline

		MYSTD::getline(in, cs); // checksum line
		fpr.Set(cs, (CSTYPES)csType, sz);

		MYSTD::getline(in, info.sPath);
		info.sPath.insert(0, m_sSrcPath+SZPATHSEP);

		in>>info.mtime;
		MYSTD::getline(in, cs); // newline

#ifdef DEBUG_FLAGS
		bool bNix=(stmiss != info.sPath.find("cabextract"));
#endif

		struct stat stbuf;
		if(0==stat(info.sPath.c_str(), &stbuf) 
				&& info.mtime==stbuf.st_mtime)
		{
			m_importMap[fpr]=info;
			m_precachedList.insert(info.sPath);
		}
		
		if(in.eof() || in.fail())
			break;
	}
	/*
	 *
	 if(m_bVerbose)
		SendChunk(ltos(m_importMap.size())+" entries<br>\n");
		*/
}


/*
void tFInfo::Dump(FILE *fh, const string & second)
{
	fprintf(fh, "%s;%lu;%lu\n%s\n", sPath.c_str(), nSize, nTime, second.c_str());
}

void pkgimport::_GetCachedKey(const string & sPath, const struct stat &stinfo, string &out)
{
	char buf[PATH_MAX+5+3*sizeof(unsigned long)];
	sprintf(buf, "%s\n%lu\n%lu\n", sPath.c_str(), (unsigned long) stinfo.st_size,
			(unsigned long) stinfo.st_mtime);
	tStringMap::const_iterator it=m_cacheMap.find(buf);
	if(it==m_cacheMap.end())
		out.clear();
	else
		out=it->second;
}

 */