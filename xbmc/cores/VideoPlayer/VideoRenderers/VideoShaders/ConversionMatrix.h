/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <array>
#include <memory>

extern "C" {
#include <libavutil/pixfmt.h>
}

template <unsigned Order>
class CMatrix
{
public:
  CMatrix() = default;
  CMatrix(CMatrix<Order - 1>& other);
  CMatrix(std::array<std::array<float, Order>, Order>& other);
  CMatrix(std::array<std::array<float, Order - 1>, Order - 1>& other);
  virtual ~CMatrix() = default;

  virtual CMatrix operator*(const std::array<std::array<float, Order>, Order>& other);

  CMatrix operator*(const CMatrix& other);
  CMatrix operator*=(const CMatrix& other);

  CMatrix& operator=(const std::array<std::array<float, Order - 1>, Order - 1>& other);

  std::array<float, Order>& operator[](int index) { return m_mat[index]; }

  std::array<std::array<float, Order>, Order>& Get();

  CMatrix Invert();

  float* ToRaw()
  {
    float* raw{nullptr};

    for (unsigned i = 0; i < Order; i++)
    {
      for (unsigned j = 0; j < Order; j++)
      {
        raw = &m_mat[i][j];
        raw++;
      }
    }

    raw -= Order * Order;
    return raw;
  }

  void SetInitialized() { m_initialized = true; }
  bool IsInitialized() { return m_initialized; }

protected:
  std::array<std::array<float, Order>, Order> Invert(
      std::array<std::array<float, Order>, Order>& other);

  std::array<std::array<float, Order>, Order> m_mat{{}};

private:
  bool m_initialized{false};
};

class CGlMatrix : public CMatrix<4>
{
public:
  CGlMatrix() = default;
  CGlMatrix(CMatrix<3>& other);
  CGlMatrix(std::array<std::array<float, 3>, 3>& other);
  ~CGlMatrix() override = default;
  CMatrix operator*(const std::array<std::array<float, 4>, 4>& other) override;
};

class CScale : public CGlMatrix
{
public:
  CScale(float x, float y, float z);
  ~CScale() override = default;
};

class CTranslate : public CGlMatrix
{
public:
  CTranslate(float x, float y, float z);
  ~CTranslate() override = default;
};

class ConversionToRGB : public CMatrix<3>
{
public:
  ConversionToRGB(float Kr, float Kb);
  ~ConversionToRGB() override = default;

protected:
  ConversionToRGB() = default;

  float a11, a12, a13;
  float CbDen, CrDen;
};

class PrimaryToXYZ : public CMatrix<3>
{
public:
  PrimaryToXYZ(const float (&primaries)[3][2], const float (&whitepoint)[2]);
  ~PrimaryToXYZ() override = default;

protected:
  PrimaryToXYZ() = default;
  float CalcBy(const float p[3][2], const float w[2]);
  float CalcGy(const float p[3][2], const float w[2], const float By);
  float CalcRy(const float By, const float Gy);
};

class PrimaryToRGB : public PrimaryToXYZ
{
public:
  PrimaryToRGB(float (&primaries)[3][2], float (&whitepoint)[2]);
  ~PrimaryToRGB() override = default;
};

//------------------------------------------------------------------------------

using Matrix4 = CMatrix<4>;
using Matrix3 = CMatrix<3>;
using Matrix3x1 = std::array<float, 3>;

class CConvertMatrix
{
public:
  CConvertMatrix() = default;
  virtual ~CConvertMatrix() = default;

  CConvertMatrix& SetSourceColorSpace(AVColorSpace colorSpace);
  CConvertMatrix& SetSourceBitDepth(int bits);
  CConvertMatrix& SetSourceLimitedRange(bool limited);
  CConvertMatrix& SetSourceTextureBitDepth(int textureBits);
  CConvertMatrix& SetSourceColorPrimaries(AVColorPrimaries src);

  CConvertMatrix& SetDestinationColorPrimaries(AVColorPrimaries dst);
  CConvertMatrix& SetDestinationContrast(float contrast);
  CConvertMatrix& SetDestinationBlack(float black);
  CConvertMatrix& SetDestinationLimitedRange(bool limited);

  Matrix4 GetYuvMat();
  Matrix3 GetPrimMat();
  float GetGammaSrc();
  float GetGammaDst();

  static Matrix3x1 GetRGBYuvCoefs(AVColorSpace colspace);

protected:
  CGlMatrix GenMat();
  CMatrix<3> GenPrimMat();

  CGlMatrix m_mat;
  CMatrix<3> m_matPrim;

  AVColorSpace m_colSpace = AVCOL_SPC_BT709;
  AVColorPrimaries m_colPrimariesSrc = AVCOL_PRI_BT709;
  float m_gammaSrc = 2.2f;
  bool m_limitedSrc = true;
  AVColorPrimaries m_colPrimariesDst = AVCOL_PRI_BT709;
  float m_gammaDst = 2.2f;
  bool m_limitedDst = false;
  int m_srcBits = 8;
  int m_srcTextureBits = 8;
  int m_dstBits = 8;
  float m_contrast = 1.0;
  float m_black = 0.0;
};
