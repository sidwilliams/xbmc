/*
 *      Copyright (c) 2007 d4rk
 *      Copyright (C) 2007-2013 Team XBMC
 *      http://kodi.tv
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include "system.h"
#include "../RenderFlags.h"
#include "YUV2RGBShaderGL.h"
#include "YUVMatrix.h"
#include "ConversionMatrix.h"
#include "ConvolutionKernels.h"
#include "settings/AdvancedSettings.h"
#include "utils/log.h"
#include "utils/GLUtils.h"

#include <string>
#include <sstream>

using namespace Shaders;

//////////////////////////////////////////////////////////////////////
// BaseYUV2RGBGLSLShader - base class for GLSL YUV2RGB shaders
//////////////////////////////////////////////////////////////////////

BaseYUV2RGBGLSLShader::BaseYUV2RGBGLSLShader(bool rect, EShaderFormat format, bool stretch,
                                             std::shared_ptr<GLSLOutput> output)
{
  m_width = 1;
  m_height = 1;
  m_field = 0;
  m_format = format;
  m_black = 0.0f;
  m_contrast = 1.0f;
  m_stretch = 0.0f;

  // get defines from the output stage if used
  m_glslOutput = output;
  if (m_glslOutput)
  {
    m_defines += m_glslOutput->GetDefines();
  }

  if(rect)
    m_defines += "#define XBMC_texture_rectangle 1\n";
  else
    m_defines += "#define XBMC_texture_rectangle 0\n";

  if(g_advancedSettings.m_GLRectangleHack)
    m_defines += "#define XBMC_texture_rectangle_hack 1\n";
  else
    m_defines += "#define XBMC_texture_rectangle_hack 0\n";

  //don't compile in stretch support when it's not needed
  if (stretch)
    m_defines += "#define XBMC_STRETCH 1\n";
  else
    m_defines += "#define XBMC_STRETCH 0\n";

  if (m_format == SHADER_YV12 ||
      m_format == SHADER_YV12_9 ||
      m_format == SHADER_YV12_10 ||
      m_format == SHADER_YV12_12 ||
      m_format == SHADER_YV12_14 ||
      m_format == SHADER_YV12_16)
    m_defines += "#define XBMC_YV12\n";
  else if (m_format == SHADER_NV12)
    m_defines += "#define XBMC_NV12\n";
  else if (m_format == SHADER_YUY2)
    m_defines += "#define XBMC_YUY2\n";
  else if (m_format == SHADER_UYVY)
    m_defines += "#define XBMC_UYVY\n";
  else if (m_format == SHADER_YV12)
    m_defines += "#define XBMC_YV12\n";
  else
    CLog::Log(LOGERROR, "GL: BaseYUV2RGBGLSLShader - unsupported format %d", m_format);

  VertexShader()->LoadSource("gl_yuv2rgb_vertex.glsl", m_defines);

  CLog::Log(LOGDEBUG, "GL: BaseYUV2RGBGLSLShader: defines:\n%s", m_defines.c_str());

  m_pConvMatrix.reset(new CConvertMatrix());
}

BaseYUV2RGBGLSLShader::~BaseYUV2RGBGLSLShader()
{
  Free();
  m_glslOutput.reset();
}

void BaseYUV2RGBGLSLShader::OnCompiledAndLinked()
{
  m_hYTex = glGetUniformLocation(ProgramHandle(), "m_sampY");
  m_hUTex = glGetUniformLocation(ProgramHandle(), "m_sampU");
  m_hVTex = glGetUniformLocation(ProgramHandle(), "m_sampV");
  m_hMatrix = glGetUniformLocation(ProgramHandle(), "m_yuvmat");
  m_hStretch = glGetUniformLocation(ProgramHandle(), "m_stretch");
  m_hStep = glGetUniformLocation(ProgramHandle(), "m_step");
  m_hVertex = glGetAttribLocation(ProgramHandle(), "m_attrpos");
  m_hYcoord = glGetAttribLocation(ProgramHandle(), "m_attrcordY");
  m_hUcoord = glGetAttribLocation(ProgramHandle(), "m_attrcordU");
  m_hVcoord = glGetAttribLocation(ProgramHandle(), "m_attrcordV");
  m_hProj = glGetUniformLocation(ProgramHandle(), "m_proj");
  m_hModel = glGetUniformLocation(ProgramHandle(), "m_model");
  m_hAlpha = glGetUniformLocation(ProgramHandle(), "m_alpha");
  VerifyGLState();

  if (m_glslOutput)
    m_glslOutput->OnCompiledAndLinked(ProgramHandle());
}

bool BaseYUV2RGBGLSLShader::OnEnabled()
{
  // set shader attributes once enabled
  glUniform1i(m_hYTex, 0);
  glUniform1i(m_hUTex, 1);
  glUniform1i(m_hVTex, 2);
  glUniform1f(m_hStretch, m_stretch);
  glUniform2f(m_hStep, 1.0 / m_width, 1.0 / m_height);

  GLfloat matrix[4][4];
  m_pConvMatrix->SetParams(m_contrast, m_black, !m_convertFullRange);
  m_pConvMatrix->GetColMajor(matrix);

  glUniformMatrix4fv(m_hMatrix, 1, GL_FALSE, (GLfloat*)matrix);
  glUniformMatrix4fv(m_hProj, 1, GL_FALSE, m_proj);
  glUniformMatrix4fv(m_hModel, 1, GL_FALSE, m_model);
  glUniform1f(m_hAlpha, m_alpha);

  VerifyGLState();
  if (m_glslOutput)
    m_glslOutput->OnEnabled();
  return true;
}

void BaseYUV2RGBGLSLShader::OnDisabled()
{
  if (m_glslOutput)
    m_glslOutput->OnDisabled();
}

void BaseYUV2RGBGLSLShader::Free()
{
  if (m_glslOutput)
    m_glslOutput->Free();
}

void BaseYUV2RGBGLSLShader::SetColSpace(AVColorSpace colSpace, AVColorPrimaries colPrimaries, int bits, bool limited,
                                        int textureBits,
                                        AVColorPrimaries destPrimaries)
{
  if (colSpace == AVCOL_SPC_UNSPECIFIED)
  {
    if (m_width > 1024 || m_height >= 600)
      colSpace = AVCOL_SPC_BT709;
    else
      colSpace = AVCOL_SPC_BT470BG;
  }
  if (colPrimaries == AVCOL_PRI_UNSPECIFIED)
  {
    if (m_width > 1024 || m_height >= 600)
      colPrimaries = AVCOL_PRI_BT709;
    else
      colPrimaries = AVCOL_PRI_BT470BG;
  }
  m_pConvMatrix->SetColSpace(colSpace, colPrimaries, bits, limited, textureBits, destPrimaries);
}

//////////////////////////////////////////////////////////////////////
// YUV2RGBProgressiveShader - YUV2RGB with no deinterlacing
// Use for weave deinterlacing / progressive
//////////////////////////////////////////////////////////////////////

YUV2RGBProgressiveShader::YUV2RGBProgressiveShader(bool rect, EShaderFormat format, bool stretch,
                                                   std::shared_ptr<GLSLOutput> output)
  : BaseYUV2RGBGLSLShader(rect, format, stretch, output)
{
  PixelShader()->LoadSource("gl_yuv2rgb_basic.glsl", m_defines);
  PixelShader()->AppendSource("gl_output.glsl");
}

//------------------------------------------------------------------------------
// YUV2RGBFilterShader4
//------------------------------------------------------------------------------

YUV2RGBFilterShader4::YUV2RGBFilterShader4(bool rect,
                                           EShaderFormat format,
                                           bool stretch,
                                           ESCALINGMETHOD method,
                                           std::shared_ptr<GLSLOutput> output)
: BaseYUV2RGBGLSLShader(rect, format, stretch, output)
{
  m_scaling = method;
  PixelShader()->LoadSource("gl_yuv2rgb_filter4.glsl", m_defines);
  PixelShader()->AppendSource("gl_output.glsl");
}

YUV2RGBFilterShader4::~YUV2RGBFilterShader4()
{
  if (m_kernelTex)
    glDeleteTextures(1, &m_kernelTex);
  m_kernelTex = 0;
}

void YUV2RGBFilterShader4::OnCompiledAndLinked()
{
  BaseYUV2RGBGLSLShader::OnCompiledAndLinked();
  m_hKernTex = glGetUniformLocation(ProgramHandle(), "m_kernelTex");

  if (m_scaling != VS_SCALINGMETHOD_LANCZOS3_FAST && m_scaling != VS_SCALINGMETHOD_SPLINE36_FAST)
  {
    CLog::Log(LOGERROR, "GL: BaseYUV2RGBGLSLShader4 - unsupported scaling %d will fallback", m_scaling);
    m_scaling = VS_SCALINGMETHOD_LANCZOS3_FAST;
  }

  CConvolutionKernel kernel(m_scaling, 256);

  if (m_kernelTex)
  {
    glDeleteTextures(1, &m_kernelTex);
    m_kernelTex = 0;
  }
  glGenTextures(1, &m_kernelTex);

  //make a kernel texture on GL_TEXTURE2 and set clamping and interpolation
  //TEXTARGET is set to GL_TEXTURE_1D or GL_TEXTURE_2D
  glActiveTexture(GL_TEXTURE3);
  glBindTexture(GL_TEXTURE_1D, m_kernelTex);
  glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

  GLvoid* data = (GLvoid*)kernel.GetFloatPixels();
  glTexImage1D(GL_TEXTURE_1D, 0, GL_RGBA32F, kernel.GetSize(), 0, GL_RGBA, GL_FLOAT, data);
  glActiveTexture(GL_TEXTURE0);
  VerifyGLState();
}

bool YUV2RGBFilterShader4::OnEnabled()
{
  glActiveTexture(GL_TEXTURE3);
  glBindTexture(GL_TEXTURE_1D, m_kernelTex);
  glUniform1i(m_hKernTex, 3);

  return BaseYUV2RGBGLSLShader::OnEnabled();
}
