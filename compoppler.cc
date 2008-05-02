/* Copyright © 2007, 2008 Jakub Wilk
 *
 * This package is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 dated June, 1991.
 */

#include <algorithm>
#include <string>
#include <iomanip>
#include <iostream>
#include <ostream>
#include <sstream>
#include <cmath>

#include "compoppler.hh"

#include <GlobalParams.h>
#include <PDFDocEncoding.h>
#include <UTF8.h>
#include <UnicodeTypeTable.h>
#if POPPLER_VERSION >= 500 && POPPLER_VERSION < 509
#include <UGooString.h>
#endif

/* class pdf::Environment
 * ======================
 */

pdf::Environment::Environment()
{
#if POPPLER_VERSION < 509
  globalParams = new GlobalParams(NULL);
#else
  globalParams = new GlobalParams();
#endif
}

bool pdf::Environment::set_antialias(bool value)
{
#if POPPLER_VERSION >= 701 && POPPLER_VERSION < 703
  // Don't allow poppler bug #15009 to appear.
  // See <https://bugs.freedesktop.org/show_bug.cgi?id=15009> for details.
  return false;
#endif
  return globalParams->setAntialias(const_cast<char*>(value ? "yes" : "no"));
#if POPPLER_VERSION >= 509
  return globalParams->setVectorAntialias(const_cast<char*>(value ? "yes" : "no"));
#endif
}


/* class pdf::Document
 * ===================
 */

pdf::Document::Document(const std::string &file_name) 
: ::PDFDoc(new pdf::String(file_name.c_str()), NULL, NULL)
{ }

static std::string html_color(const double rgb[])
{
  std::ostringstream stream;
  stream << "#";
  for (int i = 0; i < 3; i++)
    stream
      << std::setw(2) << std::setfill('0') << std::hex
      << static_cast<int>(rgb[i] * 0xff);
  return stream.str();
}

static std::string html_color(double r, double g, double b)
{
  double rgb[] = {r, g, b};
  return html_color(rgb);
}

#if POPPLER_VERSION >= 700
static void cmyk_to_rgb(double cmyk[], double rgb[])
{
  static pdf::gfx::DeviceCmykColorSpace cmyk_space;
  pdf::gfx::Color cmyk_cc;
  pdf::gfx::RgbColor rgb_cc;
  for (int i = 0; i < 4; i++)
    cmyk_cc.c[i] = pdf::gfx::double_as_color_component(cmyk[i]);
  cmyk_space.getRGB(&cmyk_cc, &rgb_cc);
  rgb[0] = pdf::gfx::color_component_as_double(rgb_cc.r);
  rgb[1] = pdf::gfx::color_component_as_double(rgb_cc.g);
  rgb[2] = pdf::gfx::color_component_as_double(rgb_cc.b);
}

static GBool annotations_callback(pdf::ant::Annotation *annotation, void *user_data)
{
  std::vector<std::string> &border_colors = *reinterpret_cast<std::vector<std::string>*>(user_data);
  std::string border_color;
  if (annotation->getType() != pdf::ant::Annotation::typeLink)
    return true;
  pdf::ant::Color *color = annotation->getColor();
  double *values = color->getValues();
  switch (color->getSpace())
  {
  case pdf::ant::Color::colorTransparent:
    break;
  case pdf::ant::Color::colorGray:
    border_color = html_color(values[0], values[0], values[0]);
    break;
  case pdf::ant::Color::colorRGB:
    border_color = html_color(values);
    break;
  case pdf::ant::Color::colorCMYK:
    {
      double rgb[3];
      cmyk_to_rgb(values, rgb);
      border_color = html_color(rgb);
    }
  }
  border_colors.push_back(border_color);
  return true;
}
#endif

void pdf::Document::display_page(pdf::Renderer *renderer, int npage, double hdpi, double vdpi, bool crop, bool do_links)
{
#if POPPLER_VERSION < 500
  this->displayPage(renderer, npage, hdpi, vdpi, 0, gFalse, do_links);
#elif POPPLER_VERSION < 509 
  this->displayPage(renderer, npage, hdpi, vdpi, 0, !crop, crop, do_links);
#elif POPPLER_VERSION < 700
  this->displayPage(renderer, npage, hdpi, vdpi, 0, !crop, crop, !do_links);
  this->processLinks(renderer, npage);
#else
  renderer->link_border_colors.clear();
  this->displayPage(renderer, npage, hdpi, vdpi, 0, !crop, crop, !do_links, 
    NULL, NULL, 
    do_links ? annotations_callback : NULL,
    do_links ? &renderer->link_border_colors : NULL
  );
  std::reverse(renderer->link_border_colors.begin(), renderer->link_border_colors.end());
  this->processLinks(renderer, npage);
#endif
}

void pdf::Document::get_page_size(int n, bool crop, double &width, double &height)
{
#if POPPLER_VERSION < 500
  width = this->getPageWidth(n);
  height = this->getPageHeight(n);
#else
  width = crop ?
      this->getPageCropWidth(n) :
      this->getPageMediaWidth(n);
  height = crop ?
      this->getPageCropHeight(n) :
      this->getPageMediaHeight(n);
#endif
  width /= 72.0;
  height /= 72.0;
  if ((this->getPageRotate(n) / 90) & 1)
    std::swap(width, height);
}


/* utility functions
 * =================
 */

void pdf::set_color(splash::Color &result, uint8_t r, uint8_t g, uint8_t b)
{
#if POPPLER_VERSION < 500
  result.rgb8 = splashMakeRGB8(r, g, b); 
#else
  result[0] = r;
  result[1] = g;
  result[2] = b;
#endif
}


/* class pdf::Renderer : pdf::splash::OutputDevice
 * ===============================================
 */

void pdf::Renderer::drawLink(pdf::link::Link *link, pdf::Catalog *catalog)
{
  std::string border_color;
#if POPPLER_VERSION < 509
  double rgb[3];
  pdf::link::BorderStyle *border_style = link->getBorderStyle();
  border_style->getColor(rgb + 0, rgb + 1, rgb + 2);
  border_color = html_color(rgb);
#else
  // FIXME: find a way to determine link color for 0.5.9 <= poppler < 0.7.0
  if (this->link_border_colors.size())
  {
    border_color = this->link_border_colors.back();
    this->link_border_colors.pop_back();
  }
#endif
  this->drawLink(link, border_color, catalog);
}


/* glyph-related functions
 * =======================
 */

bool pdf::get_glyph(splash::Splash *splash, splash::Font *font, int code, splash::GlyphBitmap *bitmap)
{
  if (font == NULL)
    return false;
#if POPPLER_VERSION >= 602
  splash::ClipResult clip_result;
  if (!font->getGlyph(code, 0, 0, bitmap, 0, 0, splash->getClip(), &clip_result))
    return false;
  return (clip_result != splashClipAllOutside);
#else
  return font->getGlyph(code, 0, 0, bitmap); 
#endif
}


/* path-related functions/methods
 * ==============================
 */

void pdf::Renderer::convert_path(pdf::gfx::State *state, splash::Path &splash_path)
{
  // copied from <PopplerOutputDev.h>
  pdf::gfx::Subpath *subpath;
  pdf::gfx::Path *path = state->getPath();
  int n_subpaths = path->getNumSubpaths();
  for (int i = 0; i < n_subpaths; i++) 
  {
    subpath = path->getSubpath(i);
    if (subpath->getNumPoints() > 0) 
    {
      double x1, y1, x2, y2, x3, y3;
      state->transform(subpath->getX(0), subpath->getY(0), &x1, &y1);
      splash_path.moveTo((splash::Coord)x1, (splash::Coord)y1);
      int j = 1;
      int n_points = subpath->getNumPoints();
      while (j < n_points)
      {
        if (subpath->getCurve(j)) 
        {
          state->transform(subpath->getX(j), subpath->getY(j), &x1, &y1);
          state->transform(subpath->getX(j + 1), subpath->getY(j + 1), &x2, &y2);
          state->transform(subpath->getX(j + 2), subpath->getY(j + 2), &x3, &y3);
          splash_path.curveTo(
            (splash::Coord)x1, (splash::Coord)y1,
            (splash::Coord)x2, (splash::Coord)y2,
            (splash::Coord)x3, (splash::Coord)y3
          );
          j += 3;
        } 
        else 
        {
          state->transform(subpath->getX(j), subpath->getY(j), &x1, &y1);
          splash_path.lineTo((splash::Coord)x1, (splash::Coord)y1);
          j++;
        }
      }
      if (subpath->isClosed())
        splash_path.close();
    }
  }
}

double pdf::get_path_area(splash::Path &path)
{
  double area = 0.0;
#if POPPLER_VERSION >= 500
  int path_len = path.getLength();
  double x0, y0;
  Guchar ch;
  path.getPoint(0, &x0, &y0, &ch);
  for (int i = 0; i < path_len - 1; i++)
  {
    double x1, y1, x2, y2;
    path.getPoint(i + 1, &x1, &y1, &ch);
    path.getPoint((i + 2) % path_len, &x2, &y2, &ch);
    x1 -= x0; y1 -= y0;
    x2 -= x0; y2 -= y0;
    area += x1 * y2 - x2 * y1;
  }
#endif
  return fabs(area);
}


/* dictionary lookup
 * =================
 */

pdf::Object *pdf::dict_lookup(pdf::Object &dict, const char *key, pdf::Object *object)
{
  return dict.dictLookup(const_cast<char*>(key), object);
}

pdf::Object *pdf::dict_lookup(pdf::Object *dict, const char *key, pdf::Object *object)
{
  return dict->dictLookup(const_cast<char*>(key), object);
}

pdf::Object *pdf::dict_lookup(pdf::Dict *dict, const char *key, pdf::Object *object)
{
  return dict->lookup(const_cast<char*>(key), object);
}




/* Unicode → UTF-8 conversion
 * ==========================
 */

void pdf::write_as_utf8(std::ostream &stream, Unicode unicode_char)
{
  char buffer[8];
  int seqlen = mapUTF8(unicode_char, buffer, sizeof buffer);
  stream.write(buffer, seqlen);
}

void pdf::write_as_utf8(std::ostream &stream, char pdf_char)
{
  write_as_utf8(stream, pdfDocEncoding[pdf_char & 0xff]);
}

void pdf::write_as_utf8(std::ostream &stream, const char *pdf_chars)
{
  for (; *pdf_chars; pdf_chars++)
    write_as_utf8(stream, *pdf_chars);
}


/* class pdf::Pixmap
 * =================
 */

namespace pdf
{
  std::ostream &operator<<(std::ostream &stream, const pdf::Pixmap &pixmap)
  {
    int height = pixmap.height;
    int width = pixmap.width;
    size_t row_size = pixmap.row_size;
    const uint8_t *row_ptr = pixmap.raw_data;
    for (int y = 0; y < height; y++)
    {
      stream.write(reinterpret_cast<const char*>(row_ptr), width * 3);
      row_ptr += row_size;
    }
    return stream;
  }
}


/* class pdf::NFKC
 * ===============
 */

pdf::NFKC::NFKC(Unicode *unistr, int length) 
#if POPPLER_VERSION < 502
: data(unistr), _length(length)
{ }
#else
: data(NULL), _length(0)
{
  data = unicodeNormalizeNFKC(unistr, length, &this->_length, NULL);
}
#endif

pdf::NFKC::~NFKC()
{
#if POPPLER_VERSION >= 502
  gfree(this->data);
#endif
}

// vim:ts=2 sw=2 et
