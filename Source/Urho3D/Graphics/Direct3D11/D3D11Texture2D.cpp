//
// Copyright (c) 2008-2015 the Urho3D project.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//

#include "../../Core/Context.h"
#include "../../IO/FileSystem.h"
#include "../../Graphics/Graphics.h"
#include "../../Graphics/GraphicsEvents.h"
#include "../../Graphics/GraphicsImpl.h"
#include "../../IO/Log.h"
#include "../../Graphics/Renderer.h"
#include "../../Core/Profiler.h"
#include "../../Resource/ResourceCache.h"
#include "../../Graphics/Texture2D.h"
#include "../../Resource/XMLFile.h"

#include "../../DebugNew.h"

namespace Urho3D
{

Texture2D::Texture2D(Context* context) :
    Texture(context)
{
}

Texture2D::~Texture2D()
{
    Release();
}

void Texture2D::RegisterObject(Context* context)
{
    context->RegisterFactory<Texture2D>();
}

bool Texture2D::BeginLoad(Deserializer& source)
{
    // In headless mode, do not actually load the texture, just return success
    if (!graphics_)
        return true;
    
    // Load the image data for EndLoad()
    loadImage_ = new Image(context_);
    if (!loadImage_->Load(source))
    {
        loadImage_.Reset();
        return false;
    }

    // Precalculate mip levels if async loading
    if (GetAsyncLoadState() == ASYNC_LOADING)
        loadImage_->PrecalculateLevels();
    
    // Load the optional parameters file
    ResourceCache* cache = GetSubsystem<ResourceCache>();
    String xmlName = ReplaceExtension(GetName(), ".xml");
    loadParameters_ = cache->GetTempResource<XMLFile>(xmlName, false);
    
    return true;
}

bool Texture2D::EndLoad()
{
    // In headless mode, do not actually load the texture, just return success
    if (!graphics_ || graphics_->IsDeviceLost())
        return true;
    
    // If over the texture budget, see if materials can be freed to allow textures to be freed
    CheckTextureBudget(GetTypeStatic());

    SetParameters(loadParameters_);
    bool success = SetData(loadImage_);
    
    loadImage_.Reset();
    loadParameters_.Reset();
    
    return success;
}

void Texture2D::Release()
{
    if (object_)
    {
        if (!graphics_)
            return;
        
        for (unsigned i = 0; i < MAX_TEXTURE_UNITS; ++i)
        {
            if (graphics_->GetTexture(i) == this)
                graphics_->SetTexture(i, 0);
        }
        
        if (renderSurface_)
            renderSurface_->Release();
        
        ((ID3D11Resource*)object_)->Release();
        object_ = 0;

        if (shaderResourceView_)
        {
            ((ID3D11ShaderResourceView*)shaderResourceView_)->Release();
            shaderResourceView_ = 0;
        }

        if (sampler_)
        {
            ((ID3D11SamplerState*)sampler_)->Release();
            sampler_ = 0;
        }
    }
    else
    {
        if (renderSurface_)
            renderSurface_->Release();
    }
}

bool Texture2D::SetSize(int width, int height, unsigned format, TextureUsage usage)
{
    // Delete the old rendersurface if any
    renderSurface_.Reset();
    usage_ = usage;
    
    if (usage_ == TEXTURE_RENDERTARGET || usage_ == TEXTURE_DEPTHSTENCIL)
    {
        renderSurface_ = new RenderSurface(this);
        // Clamp mode addressing by default, nearest filtering, and mipmaps disabled
        addressMode_[COORD_U] = ADDRESS_CLAMP;
        addressMode_[COORD_V] = ADDRESS_CLAMP;
        filterMode_ = FILTER_NEAREST;
        requestedLevels_ = 1;
    }
    
    if (usage_ == TEXTURE_RENDERTARGET)
        SubscribeToEvent(E_RENDERSURFACEUPDATE, HANDLER(Texture2D, HandleRenderSurfaceUpdate));
    else
        UnsubscribeFromEvent(E_RENDERSURFACEUPDATE);

    width_ = width;
    height_ = height;
    format_ = format;
    
    return Create();
}

bool Texture2D::SetData(unsigned level, int x, int y, int width, int height, const void* data)
{
    PROFILE(SetTextureData);
    
    if (!object_)
    {
        LOGERROR("No texture created, can not set data");
        return false;
    }
    
    if (!data)
    {
        LOGERROR("Null source for setting data");
        return false;
    }
    
    if (level >= levels_)
    {
        LOGERROR("Illegal mip level for setting data");
        return false;
    }
    
    if (IsCompressed())
    {
        x &= ~3;
        y &= ~3;
    }
    
    int levelWidth = GetLevelWidth(level);
    int levelHeight = GetLevelHeight(level);
    if (x < 0 || x + width > levelWidth || y < 0 || y + height > levelHeight || width <= 0 || height <= 0)
    {
        LOGERROR("Illegal dimensions for setting data");
        return false;
    }
    
    unsigned char* src = (unsigned char*)data;
    unsigned rowSize = GetRowDataSize(width);
    unsigned rowStart = GetRowDataSize(x);
    unsigned subResource = D3D11CalcSubresource((unsigned)level, 0, (unsigned)levels_);

    if (usage_ == TEXTURE_DYNAMIC)
    {
        if (IsCompressed())
        {
            height = (height + 3) >> 2;
            y >>= 2;
        }

        D3D11_MAPPED_SUBRESOURCE mappedData;
        mappedData.pData = 0;

        graphics_->GetImpl()->GetDeviceContext()->Map((ID3D11Resource*)object_, subResource, D3D11_MAP_WRITE_DISCARD, 0,
            &mappedData);
        if (mappedData.pData)
        {
            for (int row = 0; row < height; ++row)
                memcpy((unsigned char*)mappedData.pData + (row + y) * mappedData.RowPitch + rowStart, src + row * rowSize, rowSize);
            graphics_->GetImpl()->GetDeviceContext()->Unmap((ID3D11Resource*)object_, subResource);
        }
        else
        {
            LOGERROR("Failed to map texture for update");
            return false;
        }
    }
    else
    {
        D3D11_BOX destBox;
        destBox.left = x;
        destBox.right = x + width;
        destBox.top = y;
        destBox.bottom = y + height;
        destBox.front = 0;
        destBox.back = 1;

        graphics_->GetImpl()->GetDeviceContext()->UpdateSubresource((ID3D11Resource*)object_, subResource, &destBox, data,
            rowSize, 0);
    }

    return true;
}

bool Texture2D::SetData(SharedPtr<Image> image, bool useAlpha)
{
    if (!image)
    {
        LOGERROR("Null image, can not load texture");
        return false;
    }
    
    unsigned memoryUse = sizeof(Texture2D);
    
    int quality = QUALITY_HIGH;
    Renderer* renderer = GetSubsystem<Renderer>();
    if (renderer)
        quality = renderer->GetTextureQuality();
    
    if (!image->IsCompressed())
    {
        unsigned char* levelData = image->GetData();
        int levelWidth = image->GetWidth();
        int levelHeight = image->GetHeight();
        unsigned components = image->GetComponents();
        unsigned format = 0;
        
        // Discard unnecessary mip levels
        for (unsigned i = 0; i < mipsToSkip_[quality]; ++i)
        {
            image = image->GetNextLevel();
            levelData = image->GetData();
            levelWidth = image->GetWidth();
            levelHeight = image->GetHeight();
        }
        
        switch (components)
        {
        case 1:
            format = useAlpha ? Graphics::GetAlphaFormat() : Graphics::GetLuminanceFormat();
            break;
            
        case 2:
            format = Graphics::GetLuminanceAlphaFormat();
            break;
            
        case 3:
            format = Graphics::GetRGBFormat();
            break;
            
        case 4:
            format = Graphics::GetRGBAFormat();
            break;
        }
        
        // If image was previously compressed, reset number of requested levels to avoid error if level count is too high for new size
        if (IsCompressed() && requestedLevels_ > 1)
            requestedLevels_ = 0;
        SetSize(levelWidth, levelHeight, format);
        
        for (unsigned i = 0; i < levels_; ++i)
        {
            // D3D11 needs RGB data as 4-component
            SharedArrayPtr<unsigned char> convertedData;
            if (components == 3)
            {
                convertedData = ConvertRGBToRGBA(levelWidth, levelHeight, levelData);
                levelData = convertedData;
            }

            SetData(i, 0, 0, levelWidth, levelHeight, levelData);
            memoryUse += levelWidth * levelHeight * components;
            
            if (i < levels_ - 1)
            {
                image = image->GetNextLevel();
                levelData = image->GetData();
                levelWidth = image->GetWidth();
                levelHeight = image->GetHeight();
            }
        }
    }
    else
    {
        int width = image->GetWidth();
        int height = image->GetHeight();
        unsigned levels = image->GetNumCompressedLevels();
        unsigned format = graphics_->GetFormat(image->GetCompressedFormat());
        bool needDecompress = false;
        
        if (!format)
        {
            format = Graphics::GetRGBAFormat();
            needDecompress = true;
        }
        
        unsigned mipsToSkip = mipsToSkip_[quality];
        if (mipsToSkip >= levels)
            mipsToSkip = levels - 1;
        while (mipsToSkip && (width / (1 << mipsToSkip) < 4 || height / (1 << mipsToSkip) < 4))
            --mipsToSkip;
        width /= (1 << mipsToSkip);
        height /= (1 << mipsToSkip);
        
        SetNumLevels(Max((int)(levels - mipsToSkip), 1));
        SetSize(width, height, format);
        
        for (unsigned i = 0; i < levels_ && i < levels - mipsToSkip; ++i)
        {
            CompressedLevel level = image->GetCompressedLevel(i + mipsToSkip);
            if (!needDecompress)
            {
                SetData(i, 0, 0, level.width_, level.height_, level.data_);
                memoryUse += level.rows_ * level.rowSize_;
            }
            else
            {
                unsigned char* rgbaData = new unsigned char[level.width_ * level.height_ * 4];
                level.Decompress(rgbaData);
                SetData(i, 0, 0, level.width_, level.height_, rgbaData);
                memoryUse += level.width_ * level.height_ * 4;
                delete[] rgbaData;
            }
        }
    }
    
    SetMemoryUse(memoryUse);
    return true;
}

bool Texture2D::GetData(unsigned level, void* dest) const
{
    if (!object_)
    {
        LOGERROR("No texture created, can not get data");
        return false;
    }
    
    if (!dest)
    {
        LOGERROR("Null destination for getting data");
        return false;
    }
    
    if (level >= levels_)
    {
        LOGERROR("Illegal mip level for getting data");
        return false;
    }
    
    if (graphics_->IsDeviceLost())
    {
        LOGWARNING("Getting texture data while device is lost");
        return false;
    }
    
    int levelWidth = GetLevelWidth(level);
    int levelHeight = GetLevelHeight(level);
    
    /// \todo Implement
    return true;
}

bool Texture2D::Create()
{
    Release();
    
    if (!graphics_ || !width_ || !height_)
        return false;
    
    levels_ = CheckMaxLevels(width_, height_, requestedLevels_);
    
    D3D11_TEXTURE2D_DESC textureDesc;
    memset(&textureDesc, 0, sizeof textureDesc);
    textureDesc.Width = width_;
    textureDesc.Height = height_;
    textureDesc.MipLevels = levels_;
    textureDesc.ArraySize = 1;
    textureDesc.Format = (DXGI_FORMAT)format_;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.SampleDesc.Quality = 0;
    textureDesc.Usage = usage_ == TEXTURE_DYNAMIC ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_DEFAULT;
    textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (usage_ == TEXTURE_RENDERTARGET)
        textureDesc.BindFlags |= D3D11_BIND_RENDER_TARGET;
    else if (usage_ == TEXTURE_DEPTHSTENCIL)
        textureDesc.BindFlags |= D3D11_BIND_DEPTH_STENCIL;
    textureDesc.CPUAccessFlags = usage_ == TEXTURE_DYNAMIC ? D3D11_CPU_ACCESS_WRITE : 0;

    graphics_->GetImpl()->GetDevice()->CreateTexture2D(&textureDesc, 0, (ID3D11Texture2D**)&object_);
    if (!object_)
    {
        LOGERROR("Failed to create texture");
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC resourceViewDesc;
    memset(&resourceViewDesc, 0, sizeof resourceViewDesc);
    resourceViewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    resourceViewDesc.Texture2D.MipLevels = (unsigned)levels_;
    resourceViewDesc.Texture2D.MostDetailedMip = 0;

    graphics_->GetImpl()->GetDevice()->CreateShaderResourceView((ID3D11Resource*)object_, &resourceViewDesc,
        (ID3D11ShaderResourceView**)&shaderResourceView_);
    if (!shaderResourceView_)
    {
        LOGERROR("Failed to create shader resource view for texture");
        return false;
    }

    return true;
}

void Texture2D::HandleRenderSurfaceUpdate(StringHash eventType, VariantMap& eventData)
{
    if (renderSurface_ && renderSurface_->GetUpdateMode() == SURFACE_UPDATEALWAYS)
        renderSurface_->QueueUpdate();
}

}
