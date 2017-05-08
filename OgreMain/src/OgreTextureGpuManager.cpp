/*
-----------------------------------------------------------------------------
This source file is part of OGRE
    (Object-oriented Graphics Rendering Engine)
For the latest info, see http://www.ogre3d.org/

Copyright (c) 2000-2017 Torus Knot Software Ltd

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
-----------------------------------------------------------------------------
*/

#include "OgreTextureGpuManager.h"
#include "OgreObjCmdBuffer.h"
#include "OgreTextureGpu.h"
#include "OgreStagingTexture.h"
#include "OgrePixelFormatGpuUtils.h"

#include "OgreId.h"
#include "OgreLwString.h"
#include "OgreCommon.h"

#include "Vao/OgreVaoManager.h"
#include "OgreResourceGroupManager.h"
#include "OgreImage2.h"

#include "OgreException.h"

#define TODO_grow_pool 1

namespace Ogre
{
    inline uint32 ctz64( uint64 value )
    {
        if( value == 0 )
            return 64u;

    #if OGRE_COMPILER == OGRE_COMPILER_MSVC
        unsigned long trailingZero = 0;
        _BitScanForward64( &trailingZero, value );
        return trailingZero;
    #else
        return __builtin_ctzl( value );
    #endif
    }
    inline uint32 clz64( uint64 value )
    {
        if( value == 0 )
            return 64u;

    #if OGRE_COMPILER == OGRE_COMPILER_MSVC
        unsigned long trailingZero = 0;
        _BitScanReverse64( &trailingZero, value );
        return trailingZero;
    #else
        return __builtin_clzl( value );
    #endif
    }

    static const int c_mainThread = 0;
    static const int c_workerThread = 1;

    TextureGpuManager::TextureGpuManager( VaoManager *vaoManager ) :
        mVaoManager( vaoManager )
    {
        //64MB default
        mDefaultPoolParameters.maxBytesPerPool = 64 * 1024 * 1024;
        mDefaultPoolParameters.minSlicesPerPool[0] = 16;
        mDefaultPoolParameters.minSlicesPerPool[1] = 8;
        mDefaultPoolParameters.minSlicesPerPool[2] = 4;
        mDefaultPoolParameters.minSlicesPerPool[3] = 2;
        mDefaultPoolParameters.maxResolutionToApplyMinSlices[0] = 256;
        mDefaultPoolParameters.maxResolutionToApplyMinSlices[1] = 512;
        mDefaultPoolParameters.maxResolutionToApplyMinSlices[2] = 1024;
        mDefaultPoolParameters.maxResolutionToApplyMinSlices[3] = 4096;

        for( int i=0; i<2; ++i )
            mThreadData[i].objCmdBuffer = new ObjCmdBuffer();
    }
    //-----------------------------------------------------------------------------------
    TextureGpuManager::~TextureGpuManager()
    {
        assert( mAvailableStagingTextures.empty() && "Derived class didn't call destroyAll!" );
        assert( mUsedStagingTextures.empty() && "Derived class didn't call destroyAll!" );
        assert( mEntries.empty() && "Derived class didn't call destroyAll!" );
        assert( mTexturePool.empty() && "Derived class didn't call destroyAll!" );

        for( int i=0; i<2; ++i )
        {
            delete mThreadData[i].objCmdBuffer;
            mThreadData[i].objCmdBuffer = 0;
        }
    }
    //-----------------------------------------------------------------------------------
    void TextureGpuManager::destroyAll(void)
    {
        destroyAllStagingBuffers();
        destroyAllTextures();
        destroyAllPools();
    }
    //-----------------------------------------------------------------------------------
    void TextureGpuManager::destroyAllStagingBuffers(void)
    {
        StagingTextureVec::iterator itor = mAvailableStagingTextures.begin();
        StagingTextureVec::iterator end  = mAvailableStagingTextures.end();

        while( itor != end )
        {
            destroyStagingTextureImpl( *itor );
            delete *itor;
            ++itor;
        }

        mAvailableStagingTextures.clear();

        itor = mUsedStagingTextures.begin();
        end  = mUsedStagingTextures.end();

        while( itor != end )
        {
            destroyStagingTextureImpl( *itor );
            delete *itor;
            ++itor;
        }

        mUsedStagingTextures.clear();
    }
    //-----------------------------------------------------------------------------------
    void TextureGpuManager::destroyAllTextures(void)
    {
        ResourceEntryMap::const_iterator itor = mEntries.begin();
        ResourceEntryMap::const_iterator end  = mEntries.end();

        while( itor != end )
        {
            const ResourceEntry &entry = itor->second;
            delete entry.texture;
            ++itor;
        }

        mEntries.clear();
    }
    //-----------------------------------------------------------------------------------
    void TextureGpuManager::destroyAllPools(void)
    {
        TexturePoolList::const_iterator itor = mTexturePool.begin();
        TexturePoolList::const_iterator end  = mTexturePool.end();

        while( itor != end )
        {
            delete itor->masterTexture;
            ++itor;
        }

        mTexturePool.clear();
    }
    //-----------------------------------------------------------------------------------
    uint16 TextureGpuManager::getNumSlicesFor( TextureGpu *texture ) const
    {
        const PoolParameters &poolParams = mDefaultPoolParameters;

        uint32 maxResolution = std::max( texture->getWidth(), texture->getHeight() );
        uint16 minSlicesPerPool = 1u;

        for( int i=0; i<4; ++i )
        {
            if( maxResolution <= poolParams.maxResolutionToApplyMinSlices[i] )
            {
                minSlicesPerPool = poolParams.minSlicesPerPool[i];
                break;
            }
        }

        return minSlicesPerPool;
    }
    //-----------------------------------------------------------------------------------
    TextureGpu* TextureGpuManager::createTexture( const String &name,
                                                  GpuPageOutStrategy::GpuPageOutStrategy pageOutStrategy,
                                                  uint32 textureFlags )
    {
        IdString idName( name );

        if( mEntries.find( idName ) != mEntries.end() )
        {
            OGRE_EXCEPT( Exception::ERR_DUPLICATE_ITEM,
                         "A texture with name '" + name + "' already exists.",
                         "TextureGpuManager::createTexture" );
        }

        TextureGpu *retVal = createTextureImpl( pageOutStrategy, idName, textureFlags );

        mEntries[idName] = ResourceEntry( name, retVal );

        return retVal;
    }
    //-----------------------------------------------------------------------------------
    void TextureGpuManager::destroyTexture( TextureGpu *texture )
    {
        ResourceEntryMap::iterator itor = mEntries.find( texture->getName() );

        if( itor == mEntries.end() )
        {
            OGRE_EXCEPT( Exception::ERR_ITEM_NOT_FOUND,
                         "Texture with name '" + texture->getName().getFriendlyText() +
                         "' not found. Perhaps already destroyed?",
                         "TextureGpuManager::destroyTexture" );
        }

        delete texture;
        mEntries.erase( itor );
    }
    //-----------------------------------------------------------------------------------
    StagingTexture* TextureGpuManager::getStagingTexture( uint32 width, uint32 height,
                                                          uint32 depth, uint32 slices,
                                                          PixelFormatGpu pixelFormat )
    {
        StagingTexture *retVal = 0;
        StagingTextureVec::iterator itor = mAvailableStagingTextures.begin();
        StagingTextureVec::iterator end  = mAvailableStagingTextures.end();

        while( itor != end && !retVal )
        {
            StagingTexture *stagingTexture = *itor;

            if( stagingTexture->supportsFormat( width, height, depth, slices, pixelFormat ) )
            {
                if( !stagingTexture->uploadWillStall() )
                {
                    retVal = stagingTexture;
                    mUsedStagingTextures.push_back( stagingTexture );
                    mAvailableStagingTextures.erase( itor );
                }
            }

            ++itor;
        }

        if( !retVal )
        {
            //Couldn't find an existing StagingTexture that could handle our request. Create one.
            retVal = createStagingTextureImpl( width, height, depth, slices, pixelFormat );
            mUsedStagingTextures.push_back( retVal );
        }

        return retVal;
    }
    //-----------------------------------------------------------------------------------
    void TextureGpuManager::removeStagingTexture( StagingTexture *stagingTexture )
    {
        //Reverse search to speed up since most removals are
        //likely to remove what has just been requested.
        StagingTextureVec::reverse_iterator ritor = std::find( mUsedStagingTextures.rbegin(),
                                                               mUsedStagingTextures.rend(),
                                                               stagingTexture );
        assert( ritor != mUsedStagingTextures.rend() &&
                "StagingTexture does not belong to this TextureGpuManager or already removed" );

        StagingTextureVec::iterator itor = ritor.base() - 1u;
        efficientVectorRemove( mUsedStagingTextures, itor );

        mAvailableStagingTextures.push_back( stagingTexture );
    }
    //-----------------------------------------------------------------------------------
    const String* TextureGpuManager::findNameStr( IdString idName ) const
    {
        const String *retVal = 0;

        ResourceEntryMap::const_iterator itor = mEntries.find( idName );

        if( itor != mEntries.end() )
            retVal = &itor->second.name;

        return retVal;
    }
    //-----------------------------------------------------------------------------------
    TextureGpu* TextureGpuManager::loadFromFile( const String &name, const String &resourceGroup,
                                                 GpuPageOutStrategy::GpuPageOutStrategy pageOutStrategy,
                                                 uint32 textureFlags )
    {
        ResourceGroupManager &resourceGroupManager = ResourceGroupManager::getSingleton();
        Archive *archive = resourceGroupManager._getArchiveToResource( name, resourceGroup );

        TextureGpu *texture = createTexture( name, pageOutStrategy, textureFlags );

        ThreadData &mainData = mThreadData[c_mainThread];
        mLoadRequestsMutex.lock();
            mainData.loadRequests.push_back( LoadRequest( name, texture, archive ) );
        mLoadRequestsMutex.unlock();

        return texture;
    }
    //-----------------------------------------------------------------------------------
    void TextureGpuManager::_reserveSlotForTexture( TextureGpu *texture )
    {
        bool matchFound = false;

        TexturePoolList::iterator itor = mTexturePool.begin();
        TexturePoolList::iterator end  = mTexturePool.end();

        while( itor != end && !matchFound )
        {
            const TexturePool &pool = *itor;

            matchFound =
                    pool.hasFreeSlot() &&
                    pool.masterTexture->getWidth() == texture->getWidth() &&
                    pool.masterTexture->getHeight() == texture->getHeight() &&
                    pool.masterTexture->getDepthOrSlices() == texture->getDepthOrSlices() &&
                    pool.masterTexture->getPixelFormat() == texture->getPixelFormat() &&
                    pool.masterTexture->getNumMipmaps() == texture->getNumMipmaps();

            TODO_grow_pool;

            ++itor;
        }

        bool queueToMainThread = false;

        if( itor == end )
        {
            IdType newId = Id::generateNewId<TextureGpuManager>();
            char tmpBuffer[64];
            LwString texName( LwString::FromEmptyPointer( tmpBuffer, sizeof(tmpBuffer) ) );
            texName.a( "_InternalTex", newId );

            TexturePool newPool;
            newPool.masterTexture = createTextureImpl( GpuPageOutStrategy::Discard,
                                                       texName.c_str(), 0 );
            const uint16 numSlices = getNumSlicesFor( texture );

            newPool.usedMemory = 0;
            newPool.usedSlots.reserve( numSlices );

            newPool.masterTexture->setTextureType( TextureTypes::Type2DArray );
            newPool.masterTexture->setResolution( texture->getWidth(), texture->getHeight(), numSlices );
            newPool.masterTexture->setPixelFormat( texture->getPixelFormat() );
            newPool.masterTexture->setNumMipmaps( texture->getNumMipmaps() );

            mTexturePool.push_back( newPool );
            itor = --mTexturePool.end();

            //itor->masterTexture->transitionTo( GpuResidency::Resident, 0 );
            queueToMainThread = true;
        }

        uint16 sliceIdx = 0;
        //See if we can reuse a slot that was previously acquired and released
        if( !itor->availableSlots.empty() )
        {
            sliceIdx = itor->availableSlots.back();
            itor->availableSlots.pop_back();
        }
        else
        {
            sliceIdx = itor->usedMemory++;
        }
        itor->usedSlots.push_back( texture );
        texture->_notifyTextureSlotChanged( &(*itor), sliceIdx );

        //Must happen after _notifyTextureSlotChanged to avoid race condition.
        if( queueToMainThread )
        {
            ThreadData &workerData = mThreadData[c_workerThread];
            mPoolsPendingMutex.lock();
                workerData.poolsPending.push_back( &(*itor) );
            mPoolsPendingMutex.unlock();
        }
    }
    //-----------------------------------------------------------------------------------
    void TextureGpuManager::_releaseSlotFromTexture( TextureGpu *texture )
    {
        //const_cast? Yes. We own it. We could do a linear search to mTexturePool;
        //but it's O(N) vs O(1); and O(N) can quickly turn into O(N!).
        TexturePool *texturePool = const_cast<TexturePool*>( texture->getTexturePool() );
        TextureGpuVec::iterator itor = std::find( texturePool->usedSlots.begin(),
                                                  texturePool->usedSlots.end(), texture );
        assert( itor != texturePool->usedSlots.end() );
        efficientVectorRemove( texturePool->usedSlots, itor );

        const uint16 internalSliceStart = texture->getInternalSliceStart();
        if( texturePool->usedMemory == internalSliceStart + 1u )
            --texturePool->usedMemory;
        else
            texturePool->availableSlots.push_back( internalSliceStart );

        texture->_notifyTextureSlotChanged( 0, 0 );
    }
    //-----------------------------------------------------------------------------------
    TextureBox TextureGpuManager::getStreaming( ThreadData &workerData, const TextureBox &box,
                                                PixelFormatGpu pixelFormat,
                                                StagingTexture **outStagingTexture )
    {
        bool isRare = true;

        TextureBox retVal;

        StagingTextureVec::iterator itor = workerData.usedStagingTex.begin();
        StagingTextureVec::iterator end  = workerData.usedStagingTex.end();

        while( itor != end && !retVal.data )
        {
            retVal = (*itor)->mapRegion( box.width, box.height, box.depth, box.numSlices, pixelFormat );
            if( !retVal.data )
            {
                //If one of these staging textures supports this upload request, then it's not rare.
                isRare &= !(*itor)->supportsFormat( box.width, box.height, box.depth,
                                                    box.numSlices, pixelFormat );
            }
            else
            {
                *outStagingTexture = *itor;
                isRare = false;
            }

            ++itor;
        }

        itor = workerData.availableStagingTex.begin();
        end  = workerData.availableStagingTex.end();

        while( itor != end && !retVal.data )
        {
            retVal = (*itor)->mapRegion( box.width, box.height, box.depth, box.numSlices, pixelFormat );
            if( !retVal.data )
            {
                //If one of these staging textures supports this upload request, then it's not rare.
                isRare &= !(*itor)->supportsFormat( box.width, box.height, box.depth,
                                                    box.numSlices, pixelFormat );
                ++itor;
            }
            else
            {
                *outStagingTexture = *itor;
                isRare = false;

                //We need to move this to the 'used' textures
                workerData.usedStagingTex.push_back( *itor );
                itor = efficientVectorRemove( workerData.availableStagingTex, itor );
                end  = workerData.availableStagingTex.end();
            }
        }

        if( isRare )
        {
            bool foundMatchingRare = false;
            RareRequestVec::iterator itRare = workerData.rareRequests.begin();
            RareRequestVec::iterator enRare = workerData.rareRequests.end();

            while( itRare != enRare && !foundMatchingRare )
            {
                if( itRare->pixelFormat == pixelFormat )
                {
                    const uint32 rowAlignment = 4u;
                    size_t requiredBytes = PixelFormatGpuUtils::getSizeBytes( box.width, box.height,
                                                                              box.depth, box.numSlices,
                                                                              pixelFormat,
                                                                              rowAlignment );
                    itRare->accumSizeBytes += requiredBytes;
                    foundMatchingRare = true;
                }
                ++itRare;
            }

            if( !foundMatchingRare )
            {
                workerData.rareRequests.push_back( RareRequest( box.width, box.height,
                                                                box.getDepthOrSlices(),
                                                                pixelFormat ) );
            }
        }

        return retVal;
    }
    //-----------------------------------------------------------------------------------
    void TextureGpuManager::processQueuedImage( QueuedImage &queuedImage, ThreadData &workerData )
    {
        Image2 &img = queuedImage.image;
        TextureGpu *texture = queuedImage.dstTexture;
        ObjCmdBuffer *commandBuffer = workerData.objCmdBuffer;

        const uint8 firstMip = queuedImage.getMinMipLevel();
        const uint8 numMips = queuedImage.getMaxMipLevelPlusOne();

        for( uint8 i=firstMip; i<numMips; ++i )
        {
            if( queuedImage.isMipQueued( i ) )
            {
                TextureBox srcBox = img.getData( i );
                StagingTexture *stagingTexture = 0;
                TextureBox dstBox = getStreaming( workerData, srcBox, img.getPixelFormat(),
                                                  &stagingTexture );
                if( dstBox.data )
                {
                    //Upload to staging area. CPU -> GPU
                    memcpy( dstBox.data, srcBox.data, dstBox.bytesPerImage * dstBox.getDepthOrSlices() );

                    //Schedule a command to copy from staging to final texture, GPU -> GPU
                    ObjCmdBuffer::UploadFromStagingTex *uploadCmd = commandBuffer->addCommand<
                            ObjCmdBuffer::UploadFromStagingTex>();
                    new (uploadCmd) ObjCmdBuffer::UploadFromStagingTex( stagingTexture, dstBox,
                                                                        texture, i );
                    //This mip has been processed, flag it as done.
                    queuedImage.unqueueMip( i );
                }
            }
        }
    }
    //-----------------------------------------------------------------------------------
    void TextureGpuManager::_updateStreaming(void)
    {
        /*
        Thread Input                Thread Output
        ------------------------------------------
        Fresh StagingTextures       Used StagingTextures
        Load Requests               Filled memory
        Empty CommandBuffers        Set textures (resolution, type, pixel format)
                                    Upload commands
                                    Rare Requests

        Load Requests are protected by mLoadRequestsMutex (short lock) to prevent
        blocking main thread every time a texture is created.

        Set textures is not protected, so reading pixel format, resolution or type
        could potentially invoke a race condition.

        The rest is protected by mMutex, which takes longer. That means the worker
        thread processes a batch of textures together and when it cannot continue
        (whether it's because it ran out of space or it ran out of work) it delivers
        the commands to the main thread.
        */

        ThreadData &workerData  = mThreadData[c_workerThread];
        ThreadData &mainData    = mThreadData[c_mainThread];

        mLoadRequestsMutex.lock();
            if( workerData.loadRequests.empty() )
            {
                workerData.loadRequests.swap( mainData.loadRequests );
            }
            else
            {
                workerData.loadRequests.insert( workerData.loadRequests.end(),
                                                mainData.loadRequests.begin(),
                                                mainData.loadRequests.end() );
                mainData.loadRequests.clear();
            }
        mLoadRequestsMutex.unlock();

        mMutex.lock();

        ObjCmdBuffer *commandBuffer = workerData.objCmdBuffer;

        //First, try to upload the queued images that failed in the previous iteration.
        QueuedImageVec::iterator itQueue = mQueuedImages.begin();
        QueuedImageVec::iterator enQueue = mQueuedImages.end();

        while( itQueue != enQueue )
        {
            processQueuedImage( *itQueue, workerData );
            if( itQueue->empty() )
            {
                itQueue->destroy();
                itQueue = efficientVectorRemove( mQueuedImages, itQueue );
                enQueue = mQueuedImages.end();
            }
            else
            {
                ++itQueue;
            }
        }

        //Now process new requests from main thread
        LoadRequestVec::const_iterator itor = workerData.loadRequests.begin();
        LoadRequestVec::const_iterator end  = workerData.loadRequests.end();

        while( itor != end )
        {
            const LoadRequest &loadRequest = *itor;

            DataStreamPtr data = loadRequest.archive->open( loadRequest.name );
            {
                //Load the image from file into system RAM
                Image2 img;
                img.load( data );

                loadRequest.texture->setResolution( img.getWidth(), img.getHeight(),
                                                    img.getDepthOrSlices() );
                loadRequest.texture->setTextureType( img.getTextureType() );
                loadRequest.texture->setPixelFormat( img.getPixelFormat() );

                void *sysRamCopy = 0;
                if( loadRequest.texture->getGpuPageOutStrategy() ==
                        GpuPageOutStrategy::AlwaysKeepSystemRamCopy )
                {
                    sysRamCopy = img.getData(0).data;
                }

                //We have enough to transition the texture to Resident.
                ObjCmdBuffer::TransitionToResident *transitionCmd = commandBuffer->addCommand<
                        ObjCmdBuffer::TransitionToResident>();
                new (transitionCmd) ObjCmdBuffer::TransitionToResident( loadRequest.texture,
                                                                        sysRamCopy );

                //Queue the image for upload to GPU.
                mQueuedImages.push_back( QueuedImage( img, img.getNumMipmaps(), loadRequest.texture ) );
            }

            //Try to upload the queued image right now (all of its mipmaps).
            processQueuedImage( mQueuedImages.back(), workerData );

            if( mQueuedImages.back().empty() )
            {
                mQueuedImages.back().destroy();
                mQueuedImages.pop_back();
            }

            ++itor;
        }

        mMutex.unlock();

        workerData.loadRequests.clear();
    }
    //-----------------------------------------------------------------------------------
    void TextureGpuManager::_update(void)
    {
        ThreadData &mainData = mThreadData[c_mainThread];
        {
            ThreadData &workerData = mThreadData[c_workerThread];
            mPoolsPendingMutex.lock();
                mainData.poolsPending.swap( workerData.poolsPending );
            mPoolsPendingMutex.unlock();
            bool lockSucceeded = mMutex.tryLock();
            if( lockSucceeded )
            {
                std::swap( mainData.objCmdBuffer, workerData.objCmdBuffer );
                mainData.usedStagingTex.swap( workerData.usedStagingTex );
                workerData.availableStagingTex.insert(  );
            }
            mMutex.unlock();
        }

        {
            TexturePoolVec::const_iterator itor = mainData.poolsPending.begin();
            TexturePoolVec::const_iterator end  = mainData.poolsPending.end();

            while( itor != end )
            {
                TexturePool *pool = *itor;
                pool->masterTexture->transitionTo( GpuResidency::Resident, 0 );
                TextureGpuVec::const_iterator itTex = pool->usedSlots.begin();
                TextureGpuVec::const_iterator enTex = pool->usedSlots.end();

                while( itTex != enTex )
                {
                    (*itTex)->_notifyTextureSlotChanged( pool, (*itTex)->getInternalSliceStart() );
                    ++itTex;
                }

                ++itor;
            }

            mainData.poolsPending.clear();
        }

        {
            StagingTextureVec::iterator itor = mAvailableStagingTextures.begin();
            StagingTextureVec::iterator end  = mAvailableStagingTextures.end();

            const uint32 numFramesThreshold = mVaoManager->getDynamicBufferMultiplier() + 2u;

            //They're kept in order.
            while( itor != end &&
                   (*itor)->getLastFrameUsed() - mVaoManager->getFrameCount() > numFramesThreshold )
            {
                destroyStagingTextureImpl( *itor );
                delete *itor;
                ++itor;
            }

            mAvailableStagingTextures.erase( mAvailableStagingTextures.begin(), itor );
        }

        mainData.objCmdBuffer->execute();
        mainData.objCmdBuffer->clear();
    }
    //-----------------------------------------------------------------------------------
    //-----------------------------------------------------------------------------------
    //-----------------------------------------------------------------------------------
    bool TexturePool::hasFreeSlot(void) const
    {
        return !availableSlots.empty() || usedMemory < masterTexture->getNumSlices();
    }
    //-----------------------------------------------------------------------------------
    //-----------------------------------------------------------------------------------
    //-----------------------------------------------------------------------------------
    TextureGpuManager::RareRequest::RareRequest( uint32 _width, uint32 _height, uint32 _depthOrSlices,
                                                 PixelFormatGpu _pixelFormat ) :
        width( _width ),
        height( _height ),
        pixelFormat( _pixelFormat ),
        accumSizeBytes( PixelFormatGpuUtils::getSizeBytes( _width, _height, _depthOrSlices,
                                                           1u, _pixelFormat, 4u ) )
    {
    }
    //-----------------------------------------------------------------------------------
    //-----------------------------------------------------------------------------------
    //-----------------------------------------------------------------------------------
    TextureGpuManager::QueuedImage::QueuedImage( Image2 &srcImage, uint8 numMips,
                                                 TextureGpu *_dstTexture ) :
        dstTexture( _dstTexture )
    {
        srcImage._setAutoDelete( false );
        image = srcImage;

        for( int i=0; i<4; ++i )
        {
            if( numMips >= 64u )
            {
                mipLevelBitSet[i] = 0xffffffffffffffff;
                numMips -= 64u;
            }
            else
            {
                mipLevelBitSet[i] = (1ul << numMips) - 1ul;
            }
        }
    }
    //-----------------------------------------------------------------------------------
    void TextureGpuManager::QueuedImage::destroy(void)
    {
        if( dstTexture->getGpuPageOutStrategy() != GpuPageOutStrategy::AlwaysKeepSystemRamCopy )
        {
            image._setAutoDelete( true );
            image.freeMemory();
        }
    }
    //-----------------------------------------------------------------------------------
    bool TextureGpuManager::QueuedImage::empty(void) const
    {
        return  mipLevelBitSet[0] == 0ul && mipLevelBitSet[1] == 0ul &&
                mipLevelBitSet[2] == 0ul && mipLevelBitSet[3] == 0ul;
    }
    //-----------------------------------------------------------------------------------
    bool TextureGpuManager::QueuedImage::isMipQueued( uint8 mipLevel ) const
    {
        size_t idx  = mipLevel / 64u;
        uint64 mask = mipLevel % 64u;
        mask = 1ul << mask;
        return (mipLevelBitSet[idx] & mask) != 0;
    }
    //-----------------------------------------------------------------------------------
    void TextureGpuManager::QueuedImage::unqueueMip( uint8 mipLevel )
    {
        size_t idx  = mipLevel / 64u;
        uint64 mask = mipLevel % 64u;
        mask = 1ul << mask;
        mipLevelBitSet[idx] = mipLevelBitSet[idx] & ~mask;
    }
    //-----------------------------------------------------------------------------------
    uint8 TextureGpuManager::QueuedImage::getMinMipLevel(void) const
    {
        for( size_t i=0; i<4u; ++i )
        {
            if( mipLevelBitSet[i] != 0u )
                return static_cast<uint8>( ctz64( mipLevelBitSet[i] ) );
        }

        return 255u;
    }
    //-----------------------------------------------------------------------------------
    uint8 TextureGpuManager::QueuedImage::getMaxMipLevelPlusOne(void) const
    {
        for( size_t i=4u; i--; )
        {
            if( mipLevelBitSet[i] != 0u )
                return static_cast<uint8>( 64u - clz64( mipLevelBitSet[i] ) + 64u * i );
        }

        return 0u;
    }
}