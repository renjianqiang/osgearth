/* -*-c++-*- */
/* osgEarth - Dynamic map generation toolkit for OpenSceneGraph
 * Copyright 2020 Pelican Mapping
 * http://osgearth.org
 *
 * osgEarth is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 */
#include "FileSystemCache"
#include <osgEarth/Cache>
#include <osgEarth/StringUtils>
#include <osgEarth/Threading>
#include <osgEarth/XmlUtils>
#include <osgEarth/URI>
#include <osgEarth/FileUtils>
#include <osgEarth/StringUtils>
#include <osgEarth/Registry>
#include <osgEarth/NetworkMonitor>
#include <osgEarth/Metrics>
#include <osgDB/FileUtils>
#include <osgDB/FileNameUtils>
#include <fstream>
#include <sys/stat.h>

using namespace osgEarth;
using namespace osgEarth::Drivers;

#ifndef _WIN32
#   include <unistd.h>
#endif

#define OSG_FORMAT "osgb"
#define OSG_EXT   ".osgb"

namespace
{
    /**
     * Cache that stores data in the local file system.
     */
    class FileSystemCache : public Cache
    {
    public:
        FileSystemCache() { } // unused
        FileSystemCache( const FileSystemCache& rhs, const osg::CopyOp& op ) { } // unused
        META_Object( osgEarth, FileSystemCache );

        /**
         * Constructs a new file system cache.
         * @param options Options structure that comes from a serialized description of
         *        the object.
         */
        FileSystemCache( const CacheOptions& options );

    public: // Cache interface

        CacheBin* addBin( const std::string& binID ) override;

        CacheBin* getOrCreateDefaultBin() override;

        void setNumThreads(unsigned) override;

    protected:

        std::string _rootPath;

        osg::ref_ptr<ThreadPool> _threadPool;
    };

    struct WriteCacheRecord {
        Config meta;
        osg::ref_ptr<const osg::Object> object;
    };
    typedef std::unordered_map<std::string, WriteCacheRecord> WriteCache;

    /**
     * Cache bin implementation for a FileSystemCache.
     * You don't need to create this object directly; use FileSystemCache::createBin instead.
    */
    class FileSystemCacheBin : public CacheBin
    {
    public:
        FileSystemCacheBin( 
            const std::string& name, 
            const std::string& rootPath,
            ThreadPool* threadPool);

        static bool _s_debug;

    public: // CacheBin interface

        ReadResult readObject(const std::string& key, const osgDB::Options* dbo) override;

        ReadResult readImage(const std::string& key, const osgDB::Options* dbo) override;

        ReadResult readString(const std::string& key, const osgDB::Options* dbo) override;

        bool write(const std::string& key, const osg::Object* object, const Config& meta, const osgDB::Options* dbo) override;

        bool remove(const std::string& key) override;

        bool touch(const std::string& key) override;

        RecordStatus getRecordStatus(const std::string& key) override;

        bool clear() override;

    protected:
        bool purgeDirectory( const std::string& dir );

        bool binValidForReading(bool silent =true);

        bool binValidForWriting(bool silent =false);

        const osgDB::Options* mergeOptions(const osgDB::Options* in);

        bool                              _ok;
        bool                              _binPathExists;
        std::string                       _metaPath;       // full path to the bin's metadata file
        std::string                       _binPath;        // full path to the bin's root folder
        std::string                       _compressorName;
        osg::ref_ptr<osgDB::Options>      _zlibOptions;

        // pool for asynchronous writes
        ThreadPool* _threadPool;

    public:
        // cache for objects waiting to be written; this supports reading from
        // the cache before the object has been asynchronously written to disk.
        WriteCache _writeCache;
        ReadWriteMutex _writeCacheRWM;

        // gate to prevent multiple threads from accessing the same file
        // at the same time.
        Gate<std::string> _fileGate;

        // OSG reader-writer used to serialize the objects
        osg::ref_ptr<osgDB::ReaderWriter> _rw;
    };

    void writeMeta( const std::string& fullPath, const Config& meta )
    {
        std::ofstream outmeta( fullPath.c_str() );
        if ( outmeta.is_open() )
        {
            outmeta << meta.toJSON();
            outmeta.flush();
            outmeta.close();
        }
    }

    void readMeta( const std::string& fullPath, Config& meta )
    {
        std::ifstream inmeta( fullPath.c_str() );
        if ( inmeta.is_open() )
        {
            inmeta >> std::noskipws;
            std::stringstream buf;
            buf << inmeta.rdbuf();
            std::string bufStr;
            bufStr = buf.str();
            meta.fromJSON( bufStr );
        }
    }
}


//------------------------------------------------------------------------

#undef  LC
#define LC "[FileSystemCache] "

//#undef  OE_DEBUG
//#define OE_DEBUG OE_INFO

bool FileSystemCacheBin::_s_debug = false;

namespace
{
    FileSystemCache::FileSystemCache(const CacheOptions& options) :
        Cache(options)
    {
        FileSystemCacheOptions fsco( options );

        // read the root path from ENV is necessary:
        if ( !fsco.rootPath().isSet())
        {
            const char* cachePath = ::getenv(OSGEARTH_ENV_CACHE_PATH);
            if ( cachePath )
                fsco.rootPath() = cachePath;
        }

        _rootPath = URI( *fsco.rootPath(), options.referrer() ).full();

        if (osgDB::makeDirectory(_rootPath) == false)
        {
            _status.set(Status::ResourceUnavailable, Stringify()
                << "Failed to create or access folder \"" << _rootPath << "\"");
            return;
        }
        OE_INFO << LC << "Opened a filesystem cache at \"" << _rootPath << "\"\n";

        // create a thread pool dedicated to asynchronous cache writes
        if (fsco.threads() > 0u)
        {
            _threadPool = new ThreadPool(
                "osgEarth.FileSystemCache",
                osg::maximum(fsco.threads().get(), 1u) );
        }
    }

    void
    FileSystemCache::setNumThreads(unsigned num)
    {
        if (_threadPool.valid())
        {
            _threadPool = NULL;
        }

        if (num > 0u)
        {
            _threadPool = new ThreadPool(
                "osgEarth.FileSystemCache",
                osg::clampBetween(num, 1u, 8u));
        }
    }

    CacheBin*
    FileSystemCache::addBin( const std::string& name )
    {
        if (getStatus().isError())
            return NULL;

        return _bins.getOrCreate( name, new FileSystemCacheBin( name, _rootPath, _threadPool.get() ) );
    }

    CacheBin*
    FileSystemCache::getOrCreateDefaultBin()
    {
        if (getStatus().isError())
            return NULL;

        static Mutex s_defaultBinMutex(OE_MUTEX_NAME);
        if ( !_defaultBin.valid() )
        {
            ScopedMutexLock lock( s_defaultBinMutex );
            if ( !_defaultBin.valid() ) // double-check
            {
                _defaultBin = new FileSystemCacheBin( "__default", _rootPath, _threadPool.get() );
            }
        }
        return _defaultBin.get();
    }

    //------------------------------------------------------------------------

    bool
    FileSystemCacheBin::binValidForReading(bool silent)
    {
        if ( !_rw.valid() )
        {
            _ok = false;
        }
        else if ( !_binPathExists )
        {
            if ( osgDB::fileExists(_binPath) )
            {
                // ready to go
                _binPathExists = true;
                _ok = true;
            }
            else if ( _ok )
            {
                // one-time error.
                if ( !silent )
                {
                    OE_WARN << LC << "Failed to locate cache bin at [" << _binPath << "]" << std::endl;
                }
                _ok = false;
            }
        }

        return _ok;
    }

    bool
    FileSystemCacheBin::binValidForWriting(bool silent)
    {
        if ( !_rw.valid() )
        {
            _ok = false;
        }
        else if ( !_binPathExists )
        {
            osgEarth::makeDirectoryForFile( _metaPath );

            if ( osgDB::fileExists(_binPath) )
            {
                // ready to go
                _binPathExists = true;
                _ok = true;
            }
            else
            {
                // one-time error.
                if ( !silent )
                {
                    OE_WARN << LC << "FAILED to find or create cache bin at [" << _metaPath << "]" << std::endl;
                }
                _ok = false;
            }
        }

        return _ok;
    }

    FileSystemCacheBin::FileSystemCacheBin(
        const std::string& binID,
        const std::string& rootPath,
        ThreadPool* threadPool) :

        CacheBin(binID),
        _threadPool(threadPool),
        _binPathExists(false),
        _ok(true),
        _fileGate("CacheBinFileGate(OE)"),
        _writeCacheRWM("CacheBinWriteL2(OE)")
    {
        _binPath = osgDB::concatPaths(rootPath, binID);
        _metaPath = osgDB::concatPaths(_binPath, "osgearth_cacheinfo.json");

        _rw = osgDB::Registry::instance()->getReaderWriterForExtension(OSG_FORMAT);

        _zlibOptions = Registry::instance()->cloneOrCreateOptions();

        if (::getenv(OSGEARTH_ENV_DEFAULT_COMPRESSOR) != 0L)
        {
            _compressorName = ::getenv(OSGEARTH_ENV_DEFAULT_COMPRESSOR);
        }
        else
        {
            _compressorName = "zlib";
        }

        if (_compressorName.length() > 0)
        {
            _zlibOptions->setPluginStringData("Compressor", _compressorName);
        }

        _s_debug = ::getenv("OSGEARTH_CACHE_DEBUG") != 0L;
    }

    const osgDB::Options*
    FileSystemCacheBin::mergeOptions(const osgDB::Options* dbo)
    {
        if (!dbo)
        {
            return _zlibOptions.get();
        }
        else if (!_zlibOptions.valid())
        {
            return dbo;
        }
        else
        {
            osgDB::Options* merged = Registry::cloneOrCreateOptions(dbo);
            if (_compressorName.length())
            {
                merged->setPluginStringData("Compressor", _compressorName);
            }
            return merged;
        }
    }

    ReadResult
    FileSystemCacheBin::readImage(const std::string& key, const osgDB::Options* readOptions)
    {
        if ( !binValidForReading() )
            return ReadResult(ReadResult::RESULT_NOT_FOUND);

        // mangle "key" into a legal path name
        URI fileURI( key, _metaPath );
        std::string path = fileURI.full() + OSG_EXT;

        if ( !osgDB::fileExists(path) )
            return ReadResult( ReadResult::RESULT_NOT_FOUND );

        osgEarth::TimeStamp timeStamp = osgEarth::getLastModifiedTime(path);

        osg::ref_ptr<const osgDB::Options> dbo = mergeOptions(readOptions);

        unsigned long handle = NetworkMonitor::begin(path, "pending", "Cache");

        // lock the file:
        ScopedGate<std::string> lockFile(_fileGate, fileURI.full());

        if (_threadPool)
        {
            // first check the write-pending cache. The record will be there
            // if the object is queued for asynchronous writing but hasn't 
            // actually been saved out yet.

            ScopedReadLock lock(_writeCacheRWM);

            auto i = _writeCache.find(fileURI.full());
            if (i != _writeCache.end())
            {
                ReadResult rr(
                    const_cast<osg::Image*>(dynamic_cast<const osg::Image*>(i->second.object.get())),
                    i->second.meta);

                rr.setLastModifiedTime(timeStamp);

                NetworkMonitor::end(handle, "OK");

                return rr;
            }
        }

        osgDB::ReaderWriter::ReadResult r = _rw->readImage(path, dbo.get());
        if (!r.success())
        {
            NetworkMonitor::end(handle, "failed");
            return ReadResult();
        }
        else
        {
            NetworkMonitor::end(handle, "OK");
        }

        // read metadata
        Config meta;
        std::string metafile = fileURI.full() + ".meta";
        if (osgDB::fileExists(metafile))
            readMeta(metafile, meta);

        ReadResult rr(r.getImage(), meta);
        rr.setLastModifiedTime(timeStamp);

        if (_s_debug)
            OE_NOTICE << LC << "Read image \"" << key << "\" from cache bin [" << getID() << "] path=" << fileURI.full() << "." << OSG_EXT << std::endl;

        return rr;
    }

    ReadResult
    FileSystemCacheBin::readObject(const std::string& key, const osgDB::Options* readOptions)
    {
        if ( !binValidForReading() )
            return ReadResult(ReadResult::RESULT_NOT_FOUND);

        // mangle "key" into a legal path name
        URI fileURI( key, _metaPath );
        std::string path = fileURI.full() + OSG_EXT;

        if ( !osgDB::fileExists(path) )
            return ReadResult( ReadResult::RESULT_NOT_FOUND );

        osgEarth::TimeStamp timeStamp = osgEarth::getLastModifiedTime(path);

        osg::ref_ptr<const osgDB::Options> dbo = mergeOptions(readOptions);

        unsigned long handle = NetworkMonitor::begin(path, "pending", "Cache");

        // lock the file:
        ScopedGate<std::string> lockFile(_fileGate, fileURI.full());

        if (_threadPool)
        {
            // first check the write-pending cache. The record will be there
            // if the object is queued for asynchronous writing but hasn't 
            // actually been saved out yet.
        
            ScopedReadLock lock(_writeCacheRWM);

            auto i = _writeCache.find(fileURI.full());
            if (i != _writeCache.end())
            {
                ReadResult rr(
                    const_cast<osg::Object*>(i->second.object.get()),
                    i->second.meta);

                rr.setLastModifiedTime(timeStamp);

                NetworkMonitor::end(handle, "OK");

                return rr;
            }
        }

        osgDB::ReaderWriter::ReadResult r = _rw->readObject(path, dbo.get());
        if (!r.success())
        {
            NetworkMonitor::end(handle, "failed");
            return ReadResult();
        }
        else
        {
            NetworkMonitor::end(handle, "OK");
        }

        // read metadata
        Config meta;
        std::string metafile = fileURI.full() + ".meta";
        if (osgDB::fileExists(metafile))
            readMeta(metafile, meta);

        ReadResult rr(r.getObject(), meta);
        rr.setLastModifiedTime(timeStamp);

        if (_s_debug)
            OE_NOTICE << LC << "Read object \"" << key << "\" from cache bin [" << getID() << "] path=" << fileURI.full() << "." << OSG_EXT << std::endl;

        return rr;
    }

    ReadResult
    FileSystemCacheBin::readString(const std::string& key, const osgDB::Options* readOptions)
    {
        ReadResult r = readObject(key, readOptions);
        if ( r.succeeded() )
        {
            if ( r.get<StringObject>() )
            {
                if (_s_debug)
                    OE_NOTICE << LC << "Read string \"" << key << "\" from cache bin [" << getID() << "]" << std::endl;

                return r;
            }
            else
            {
                return ReadResult();
            }
        }
        else
        {
            return r;
        }
    }

    namespace
    {
        struct WriteOperation : public osg::Operation
        {
        public:
            WriteOperation(
                const URI& uri,
                const osg::Object* object,
                const Config& meta,
                const osgDB::Options* writeOptions,
                FileSystemCacheBin* bin) :

                osg::Operation(bin->getID(), false),
                _uri(uri),
                _object(object),
                _meta(meta),
                _writeOptions(writeOptions),
                _bin(bin)
            {
                //nop
            }

            void operator()(osg::Object*) override
            {
                OE_PROFILING_ZONE_NAMED("FS Cache Write");

                // prevent more than one thread from writing to the same key at the same time
                ScopedGate<std::string> lockFile(_bin->_fileGate, _uri.full());

                // make a home for it..
                if (!osgDB::fileExists(osgDB::getFilePath(_uri.full())))
                {
                    osgEarth::makeDirectoryForFile(_uri.full());
                }

                osgDB::ReaderWriter::WriteResult r;

                bool writeOK = false;

                if (dynamic_cast<const osg::Image*>(_object.get()))
                {
                    std::string filename = _uri.full() + OSG_EXT;
                    r = _bin->_rw->writeImage(*static_cast<const osg::Image*>(_object.get()), filename, _writeOptions.get());
                    writeOK = r.success();
                }
                else if (dynamic_cast<const osg::Node*>(_object.get()))
                {
                    std::string filename = _uri.full() + OSG_EXT;
                    r = _bin->_rw->writeNode(*static_cast<const osg::Node*>(_object.get()), filename, _writeOptions.get());
                    writeOK = r.success();
                }
                else
                {
                    std::string filename = _uri.full() + OSG_EXT;
                    r = _bin->_rw->writeObject(*_object.get(), filename, _writeOptions.get());
                    writeOK = r.success();
                }

                // write metadata
                if (!_meta.empty() && writeOK)
                {
                    std::string metaname = _uri.full() + ".meta";
                    writeMeta(metaname, _meta);
                }

                if (!writeOK)
                {
                    OE_WARN << LC << "FAILED to write \"" << _uri.full() << "\" to cache bin \"" << 
                        _bin->getID() << "\"; msg = \"" << r.message() << "\"" << std::endl;
                }
                else
                {
                    OE_DEBUG << LC << "Wrote " << _uri.full() << " to cache bin " << _bin->getID() << std::endl;
                }

                // remove it from the write cache now that we're done.
                {
                    ScopedWriteLock lock(_bin->_writeCacheRWM);
                    _bin->_writeCache.erase(_uri.full());
                }
            }

        private:
            URI _uri;
            osg::ref_ptr<const osg::Object> _object;
            Config _meta;
            osg::ref_ptr<const osgDB::Options> _writeOptions;
            osg::ref_ptr<FileSystemCacheBin> _bin;
        };
    }

    bool
    FileSystemCacheBin::write(
        const std::string& key, 
        const osg::Object* object, 
        const Config& meta, 
        const osgDB::Options* writeOptions)
    {
        if ( !binValidForWriting() || !object )
            return false;

        // convert the key into a legal filename:
        URI fileURI( key, _metaPath );

        // combine custom options with cache options:
        osg::ref_ptr<const osgDB::Options> dbo = mergeOptions(writeOptions);

        // Temporary: Check whether it's a node because we can't thread
        // out the NODE writes until we figure out the thread-safety 
        // issue and make all the reads return CONST objects
        bool isNode = dynamic_cast<const osg::Node*>(object) != nullptr;

        if (_threadPool && !isNode)
        {
            // Store in the write-cache until it's actually written.
            // Will override any existing entry and that's OK since the 
            // most recent one is the valid one.
            _writeCacheRWM.write_lock();
            WriteCacheRecord& record = _writeCache[fileURI.full()];
            record.meta = meta;
            record.object = object;
            _writeCacheRWM.write_unlock();

            // queue the asynchronous write.
            WriteOperation* writer = new WriteOperation(
                fileURI,
                object,
                meta,
                dbo.get(),
                this);

            _threadPool->run(writer);
        }
        else // synchronous write:
        { 
            WriteOperation writeOp(
                fileURI,
                object,
                meta,
                dbo.get(),
                this);

            writeOp.operator()(nullptr);
        }

        return true;
    }

    CacheBin::RecordStatus
    FileSystemCacheBin::getRecordStatus(const std::string& key)
    {
        if ( !binValidForReading() )
            return STATUS_NOT_FOUND;

        URI fileURI( key, _metaPath );
        std::string path( fileURI.full() + OSG_EXT );
        if ( !osgDB::fileExists(path) )
            return STATUS_NOT_FOUND;

        return STATUS_OK;
    }

    bool
    FileSystemCacheBin::remove(const std::string& key)
    {
        if ( !binValidForReading() ) return false;
        URI fileURI( key, _metaPath );
        std::string path( fileURI.full() + OSG_EXT );

        // exclusive file access:
        ScopedGate<std::string> lockFile(_fileGate, fileURI.full());
        return ::unlink( path.c_str() ) == 0;
    }

    bool
    FileSystemCacheBin::touch(const std::string& key)
    {
        if ( !binValidForReading() ) return false;
        URI fileURI( key, _metaPath );
        std::string path( fileURI.full() + OSG_EXT );

        // exclusive file access:
        ScopedGate<std::string> lockFile(_fileGate, fileURI.full());
        return osgEarth::touchFile( path );
    }

    bool
    FileSystemCacheBin::purgeDirectory( const std::string& dir )
    {
        if ( !binValidForReading() ) return false;

        bool allOK = true;
        osgDB::DirectoryContents dc = osgDB::getDirectoryContents( dir );

        for( osgDB::DirectoryContents::iterator i = dc.begin(); i != dc.end(); ++i )
        {
            int ok = 0;
            std::string full = osgDB::concatPaths(dir, *i);

            if ( full.find( getID() ) != std::string::npos ) // safety latch
            {
                osgDB::FileType type = osgDB::fileType( full );

                if ( type == osgDB::DIRECTORY && i->compare(".") != 0 && i->compare("..") != 0 )
                {
                    purgeDirectory( full );

                    ok = ::unlink( full.c_str() );
                    if (_s_debug)
                        OE_NOTICE << LC << "Unlink: " << full << std::endl;
                }
                else if ( type == osgDB::REGULAR_FILE )
                {
                    if ( full != _metaPath )
                    {
                        ok = ::unlink( full.c_str() );
                        if (_s_debug)
                            OE_NOTICE << LC << "Unlink: " << full << std::endl;
                    }
                }

                if ( ok != 0 )
                    allOK = false;
            }
        }

        return allOK;
    }

    bool
    FileSystemCacheBin::clear()
    {
        if ( !binValidForReading() )
            return false;

        std::string binDir = osgDB::getFilePath( _metaPath );
        return purgeDirectory( binDir );
    }
}

//------------------------------------------------------------------------

/**
 * This driver defers loading of the source data to the appropriate OSG plugin. You
 * must explicitly set an override profile when using this driver.
 *
 * For example, use this driver to load a simple jpeg file; then set the profile to
 * tell osgEarth its projection.
 */
class FileSystemCacheDriver : public CacheDriver
{
public:
    FileSystemCacheDriver()
    {
        supportsExtension( "osgearth_cache_filesystem", "File system cache for osgEarth" );
    }

    virtual const char* className() const
    {
        return "File system cache for osgEarth";
    }

    virtual ReadResult readObject(const std::string& file_name, const Options* options) const
    {
        if ( !acceptsExtension(osgDB::getLowerCaseFileExtension( file_name )))
            return ReadResult::FILE_NOT_HANDLED;

        return ReadResult( new FileSystemCache( getCacheOptions(options) ) );
    }
};

REGISTER_OSGPLUGIN(osgearth_cache_filesystem, FileSystemCacheDriver)
