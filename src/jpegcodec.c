/* -*- Mode: c; c-basic-offset: 4; indent-tabs-mode: t -*-
 *
 * jpegcodec.c : Contains function definitions for encoding decoding jpeg images
 *
 * Copyright (C) Novell, Inc. 2003-2004.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software 
 * and associated documentation files (the "Software"), to deal in the Software without restriction, 
 * including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, 
 * and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, 
 * subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in all copies or substantial 
 * portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT 
 * NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. 
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, 
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE 
 * OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *  	Sanjay Gupta (gsanjay@novell.com)
 *      Vladimir Vukicevic (vladimir@pobox.com)
 *
 */

#if HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include "jpegcodec.h"
#include "gdipImage.h"

#ifdef HAVE_LIBJPEG

#include <setjmp.h>

/* pkgsrc */
#undef HAVE_STDLIB_H
#include <jpeglib.h>
#include "dstream.h"
#ifdef HAVE_LIBEXIF
#include <libexif/exif-data.h>
#include <libexif/exif-content.h>
#include <libexif/exif-entry.h>
#endif


/* Codecinfo related data*/
static ImageCodecInfo jpeg_codec;
static const WCHAR jpeg_codecname[] = {'B', 'u', 'i','l', 't', '-','i', 'n', ' ', 'J', 'P', 'E', 'G', 0}; /* Built-in JPEG */
static const WCHAR jpeg_extension[] = {'*', '.', 'J', 'P','G', ';','*', '.', 'J','P', 'E', 'G', ';', '*',
        '.', 'J', 'P', 'E', ';', '*', '.', 'J', 'F','I','F', 0}; /* JPG;*.JPEG;*.JPE;*.JFIF */
static const WCHAR jpeg_mimetype[] = {'i', 'm', 'a','g', 'e','/', 'j', 'p', 'e', 'g', 0}; /* image/png */
static const WCHAR jpeg_format[] = {'J', 'P', 'E', 'G', 0}; /* JPEG */


ImageCodecInfo *
gdip_getcodecinfo_jpeg ()
{
	jpeg_codec.Clsid = (CLSID) { 0x557cf401, 0x1a04, 0x11d3, { 0x9a, 0x73, 0x0, 0x0, 0xf8, 0x1e, 0xf3, 0x2e } };
	jpeg_codec.FormatID = gdip_jpg_image_format_guid;
	jpeg_codec.CodecName = (const WCHAR*)  jpeg_codecname;
	jpeg_codec.DllName = NULL;
	jpeg_codec.FormatDescription = (const WCHAR*) jpeg_format;
	jpeg_codec.FilenameExtension = (const WCHAR*) jpeg_extension;
	jpeg_codec.MimeType = (const WCHAR*) jpeg_mimetype;
	jpeg_codec.Flags = Encoder | Decoder | SupportBitmap | Builtin;
	jpeg_codec.Version = 1;
	jpeg_codec.SigCount = 0;
	jpeg_codec.SigSize = 0;
	jpeg_codec.SigPattern = 0;
	jpeg_codec.SigMask = 0;

	return &jpeg_codec;
}


#if !defined(HAVE_SIGSETJMP) && !defined(sigsetjmp)
#define sigjmp_buf jmp_buf
#define sigsetjmp(jb, x) setjmp(jb)
#define siglongjmp longjmp
#endif

#define JPEG_BUFFER_SIZE	65536

struct gdip_stdio_jpeg_source_mgr {
	struct jpeg_source_mgr parent;

	FILE *infp;
	JOCTET *buf;
};
typedef struct gdip_stdio_jpeg_source_mgr *gdip_stdio_jpeg_source_mgr_ptr;

struct gdip_stream_jpeg_source_mgr {
	struct jpeg_source_mgr parent;
	dstream_t *loader;

	JOCTET *buf;
};
typedef struct gdip_stream_jpeg_source_mgr *gdip_stream_jpeg_source_mgr_ptr;

struct gdip_stream_jpeg_dest_mgr {
	struct jpeg_destination_mgr parent;

	PutBytesDelegate putBytesFunc;

	JOCTET *buf;
};
typedef struct gdip_stream_jpeg_dest_mgr *gdip_stream_jpeg_dest_mgr_ptr;

struct gdip_jpeg_error_mgr {
	struct jpeg_error_mgr parent;
	sigjmp_buf setjmp_buffer;
	GError **error;
};
typedef struct gdip_jpeg_error_mgr *gdip_jpeg_error_mgr_ptr;


static void
_gdip_jpeg_error_exit (j_common_ptr cinfo)
{
	/*gdip_jpeg_error_mgr_ptr err = (gdip_jpeg_error_mgr_ptr) cinfo->err;*/
	char buffer[JMSG_LENGTH_MAX];

	(* cinfo->err->format_message) (cinfo, buffer);

	/* There is some very bizzare stuff happening with error
	 * reporting.  These are supposed to be fatal errors,
	 * but libjpeg goes through a bunch of these calls and then
	 * -finishes uncompressing the image-.  I have no idea
	 * what's going on here.
	 * -- FIXME
	 */
#if 0
	printf ("jpeg error: %d %s\n", err->parent.msg_code, buffer);
	siglongjmp (err->setjmp_buffer, 1);
#endif
}

static void
_gdip_jpeg_output_message (j_common_ptr cinfo)
{
	/* shh. */
}

static void
_gdip_source_dummy_init (j_decompress_ptr cinfo)
{
	/* nothing */
}

static bool
_gdip_source_stdio_fill_input_buffer (j_decompress_ptr cinfo)
{
	gdip_stdio_jpeg_source_mgr_ptr src = (gdip_stdio_jpeg_source_mgr_ptr) cinfo->src;
	size_t nb;

	nb = fread (src->buf, 1, JPEG_BUFFER_SIZE, src->infp);

	if (nb <= 0) {
		/* this is a hack learned from gdk-pixbuf */
		/* insert fake EOI marker, to try to salvage image
		 * in case of malformed/incomplete input */
		src->buf[0] = (JOCTET) 0xFF;
		src->buf[1] = (JOCTET) JPEG_EOI;
		nb = 2;
	}

	src->parent.next_input_byte = src->buf;
	src->parent.bytes_in_buffer = nb;

	return TRUE;
}

static void
_gdip_source_stdio_skip_input_data (j_decompress_ptr cinfo, long skipbytes)
{
	gdip_stdio_jpeg_source_mgr_ptr src = (gdip_stdio_jpeg_source_mgr_ptr) cinfo->src;

	if (skipbytes > 0) {
		if (skipbytes > (long) src->parent.bytes_in_buffer) {
			skipbytes -= (long) src->parent.bytes_in_buffer;
			fseek (src->infp, skipbytes, SEEK_CUR);

			(void) _gdip_source_stdio_fill_input_buffer (cinfo);
		} else {
			src->parent.next_input_byte += (size_t) skipbytes;
			src->parent.bytes_in_buffer -= (size_t) skipbytes;
		}
	}
}

static bool
_gdip_source_stream_fill_input_buffer (j_decompress_ptr cinfo)
{
	gdip_stream_jpeg_source_mgr_ptr src = (gdip_stream_jpeg_source_mgr_ptr) cinfo->src;
	dstream_t *loader = src->loader;
	size_t nb;

	nb = dstream_read (loader, src->buf, JPEG_BUFFER_SIZE, 0);
	if (nb <= 0) {
		/* this is a hack learned from gdk-pixbuf */
		/* insert fake EOI marker, to try to salvage image
		 * in case of malformed/incomplete input */
		src->buf[0] = (JOCTET) 0xFF;
		src->buf[1] = (JOCTET) JPEG_EOI;
		nb = 2;
	}

	src->parent.next_input_byte = src->buf;
	src->parent.bytes_in_buffer = nb;

	return TRUE;
}

static void
_gdip_source_stream_skip_input_data (j_decompress_ptr cinfo, long skipbytes)
{
	gdip_stream_jpeg_source_mgr_ptr src = (gdip_stream_jpeg_source_mgr_ptr) cinfo->src;
	dstream_t *loader = src->loader;

	if (skipbytes > 0) {
		if (skipbytes > (long) src->parent.bytes_in_buffer) {
			skipbytes -= (long) src->parent.bytes_in_buffer;
			dstream_skip (loader, skipbytes);
			(void) _gdip_source_stream_fill_input_buffer (cinfo);
		} else {
			src->parent.next_input_byte += (size_t) skipbytes;
			src->parent.bytes_in_buffer -= (size_t) skipbytes;
		}
	}
}

static void
_gdip_source_dummy_term (j_decompress_ptr cinfo)
{
	/* nothing */
}

static void
_gdip_dest_stream_init (j_compress_ptr cinfo)
{
	gdip_stream_jpeg_dest_mgr_ptr dest = (gdip_stream_jpeg_dest_mgr_ptr) cinfo->dest;
	dest->parent.next_output_byte = dest->buf;
	dest->parent.free_in_buffer = JPEG_BUFFER_SIZE;
}

static BOOL
_gdip_dest_stream_empty_output_buffer (j_compress_ptr cinfo)
{
	gdip_stream_jpeg_dest_mgr_ptr dest = (gdip_stream_jpeg_dest_mgr_ptr) cinfo->dest;

	dest->putBytesFunc (dest->buf, JPEG_BUFFER_SIZE);

	dest->parent.next_output_byte = dest->buf;
	dest->parent.free_in_buffer = JPEG_BUFFER_SIZE;

	return TRUE;
}

static void
_gdip_dest_stream_term (j_compress_ptr cinfo)
{
	gdip_stream_jpeg_dest_mgr_ptr dest = (gdip_stream_jpeg_dest_mgr_ptr) cinfo->dest;

	dest->putBytesFunc (dest->buf, JPEG_BUFFER_SIZE - dest->parent.free_in_buffer);
}

GpStatus
gdip_load_jpeg_image_internal (struct jpeg_source_mgr *src, GpImage **image)
{
	struct jpeg_decompress_struct	cinfo;
	struct gdip_jpeg_error_mgr	jerr;
	GpBitmap	*result;
	guchar		*destbuf;
	guchar		*destptr;
	guchar		*lines[4] = {NULL, NULL, NULL, NULL};
	GpStatus	status;
	int		stride;

	destbuf = NULL;
	result = NULL;

	cinfo.err = jpeg_std_error ((struct jpeg_error_mgr *) &jerr);
	jerr.parent.error_exit = _gdip_jpeg_error_exit;
	jerr.parent.output_message = _gdip_jpeg_output_message;

	if (sigsetjmp (jerr.setjmp_buffer, 1)) {
		/* Error occured during decompression */
		status = GenericError;
		goto error;
	}

	jpeg_create_decompress (&cinfo);
	cinfo.src = src;

	jpeg_read_header (&cinfo, TRUE);

	cinfo.do_fancy_upsampling = FALSE;
	cinfo.do_block_smoothing = FALSE;

	result = gdip_bitmap_new_with_frame (NULL, TRUE);
	result->type = imageBitmap;
	result->active_bitmap->width = cinfo.image_width;
	result->active_bitmap->height = cinfo.image_height;

	if (cinfo.density_unit == 1) { /* dpi */
		result->active_bitmap->dpi_horz = cinfo.X_density;
		result->active_bitmap->dpi_vert = cinfo.Y_density;
	} else if (cinfo.density_unit == 2) { /* dots/cm */
		result->active_bitmap->dpi_horz = cinfo.X_density * 2.54;
		result->active_bitmap->dpi_vert = cinfo.Y_density * 2.54;
	} else { /* unknown density */
		result->active_bitmap->dpi_horz = 0;
		result->active_bitmap->dpi_vert = 0;
	}

	if (cinfo.num_components == 1) {
		result->active_bitmap->pixel_format = Format8bppIndexed;
	} else if (cinfo.num_components == 3) {
		/* libjpeg gives us RGB for many formats and
		 * we convert to RGB format when needed. JPEG
		 * does not support alpha (transparency). */
		result->active_bitmap->pixel_format = Format24bppRgb;
	} else if (cinfo.num_components == 4) {
		result->active_bitmap->pixel_format = Format32bppRgb;
	}

	switch (cinfo.jpeg_color_space) {
		case JCS_GRAYSCALE:
			result->active_bitmap->image_flags = ImageFlagsColorSpaceGRAY;
			break;

		case JCS_RGB:
			result->active_bitmap->image_flags = ImageFlagsColorSpaceRGB;
			break;

		case JCS_YCbCr:
			result->active_bitmap->image_flags = ImageFlagsColorSpaceYCBCR;
			break;

		case JCS_YCCK:
			result->active_bitmap->image_flags = ImageFlagsColorSpaceYCCK;
			break;

		case JCS_CMYK:
			result->active_bitmap->image_flags = ImageFlagsColorSpaceCMYK;
			break;

		default:
			result->active_bitmap->image_flags = ImageFlagsNone; /* Unknown Colorspace */
	}

	result->cairo_format = CAIRO_FORMAT_ARGB32;
	result->active_bitmap->stride = 4 * cinfo.image_width;

	stride = result->active_bitmap->stride;

	/* Request cairo-compat output */
	/* libjpeg can do only following conversions,
	 * YCbCr => GRAYSCALE, YCbCr => RGB
	 * GRAYSCALE => RGB, YCCK => CMYK.
	 * Therefore, we convert YCbCr, GRAYSCALE to RGB and
	 * YCCK to CMYK using the libjpeg. We convert CMYK
	 * to RGB ourself.
	 */
	if (cinfo.jpeg_color_space == JCS_RGB || cinfo.jpeg_color_space == JCS_YCbCr || cinfo.jpeg_color_space == JCS_GRAYSCALE) {
		cinfo.out_color_space = JCS_RGB;
		cinfo.out_color_components = 3;
	} else if (cinfo.jpeg_color_space == JCS_YCCK ||cinfo.jpeg_color_space == JCS_CMYK) {
		cinfo.out_color_space = JCS_CMYK;
		cinfo.out_color_components = 4;
	} else {
		/* Unsupported JPEG color space */
		status = InvalidParameter;
		goto error;
	}

	jpeg_start_decompress (&cinfo);

	destbuf = GdipAlloc (stride * cinfo.output_height);
	if (destbuf == NULL) {
		status = OutOfMemory;
		goto error;
	}
	destptr = destbuf;

	while (cinfo.output_scanline < cinfo.output_height) {
		int i;
		int nlines;
		for (i = 0; i < cinfo.rec_outbuf_height; i++) {
			lines[i] = destptr;
			destptr += stride;
		}

		nlines = jpeg_read_scanlines (&cinfo, lines, cinfo.rec_outbuf_height);

		/* If the out colorspace is not RBG, we need to convert it to RBG. */
		if (cinfo.out_color_space == JCS_CMYK) {
			int i, j;

			for (i = 0; i < cinfo.rec_outbuf_height; i++) {
				guchar *lineptr;

				lineptr = lines [i];

				for (j = 0; j < cinfo.output_width; j++) {
					JOCTET c, m, y, k;
					JOCTET r, g, b;

					c = lineptr [0];
					m = lineptr [1];
					y = lineptr [2];
					k = lineptr [3];

					/* Adobe photoshop seems to have a bug and inverts the CMYK data.
					 * We might need to remove this check, if Adobe decides to fix it. */
					if (cinfo.saw_Adobe_marker) {
						b = (k * c) / 255;
						g = (k * m) / 255;
						r = (k * y) / 255;
					}  else {
						b = (255 - k) * (255 - c) / 255;
						g = (255 - k) * (255 - m) / 255;
						r = (255 - k) * (255 - y) / 255;
					}

					set_pixel_bgra(lineptr, 0, b, g, r, 0xff);
					lineptr += 4;
				}
			}
		} else {
			for (i = 0; i < nlines; i++) {
				int j;
				guchar *inptr, *outptr;

				inptr = lines[i] + (result->active_bitmap->width) * 3;
				outptr = lines[i] + stride;
				for (j = 0; j < result->active_bitmap->width; j++) {
					/* Note the swapping of R and B, to get ARGB from what
					 * looks like BGR data. */
					inptr -= 3;
					outptr -= 4;
					set_pixel_bgra(outptr, 0, inptr[2], inptr[1], inptr[0], 0xff);
				}
			}
		}
	}

	jpeg_finish_decompress (&cinfo);
	jpeg_destroy_decompress (&cinfo);

	result->active_bitmap->scan0 = destbuf;
	result->active_bitmap->reserved = GBD_OWN_SCAN0;

	result->surface = cairo_image_surface_create_for_data ((unsigned char *)destbuf, result->cairo_format,
		result->active_bitmap->width, result->active_bitmap->height, stride);

	/* win32 returns this as PartiallyScalable and ColorSpaceYCBCR; we
	 * just return it as RGB (or Grayscale). */
	result->active_bitmap->image_flags |= ImageFlagsReadOnly | ImageFlagsHasRealPixelSize;

	*image = result;
	return Ok;

error:
	jpeg_destroy_decompress (&cinfo);

	if (destbuf != NULL) {
		GdipFree (destbuf);
	}

	if (result != NULL) {
		gdip_bitmap_dispose (result);
	}
	*image = NULL;
	return status;
}

#ifdef HAVE_LIBEXIF
static void
add_properties_from_entry (ExifEntry *entry, void *user_data)
{
	BitmapData *bitmap_data = (BitmapData *) user_data;
	ULONG length;

	gdip_bitmapdata_property_add (bitmap_data, entry->tag, entry->size, entry->format, entry->data);
}

static void
add_properties_from_content (ExifContent *content, void *user_data)
{
	exif_content_foreach_entry (content, add_properties_from_entry, user_data);
}

static void
load_exif_data (ExifData *exif_data, GpImage *image)
{
	int n;
	BitmapData *bitmap;

	if (exif_data == NULL)
		return;

	bitmap = image->active_bitmap;
	exif_data_foreach_content (exif_data, add_properties_from_content, bitmap);
	/* thumbnail */
	if (exif_data->size != 0) {
		gdip_bitmapdata_property_add (bitmap, ThumbnailData, exif_data->size, TypeByte, exif_data->data);
	}
	exif_data_unref (exif_data);
}
#endif

GpStatus 
gdip_load_jpeg_image_from_file (FILE *fp, const char *filename, GpImage **image)
{
	GpStatus st;
#ifdef HAVE_LIBEXIF
	ExifData *exif_data;
#endif

	gdip_stdio_jpeg_source_mgr_ptr src;

	src = (gdip_stdio_jpeg_source_mgr_ptr) GdipAlloc (sizeof (struct gdip_stdio_jpeg_source_mgr));
	if (src == NULL) {
		return OutOfMemory;
	}

	src->buf = GdipAlloc (JPEG_BUFFER_SIZE * sizeof(JOCTET));
	if (src->buf == NULL) {
		GdipFree(src);
		return OutOfMemory;
	}

	src->parent.init_source = _gdip_source_dummy_init;
	src->parent.fill_input_buffer = _gdip_source_stdio_fill_input_buffer;
	src->parent.skip_input_data = _gdip_source_stdio_skip_input_data;
	src->parent.resync_to_restart = jpeg_resync_to_restart;
	src->parent.term_source = _gdip_source_dummy_term;
	src->parent.bytes_in_buffer = 0;
	src->parent.next_input_byte = NULL;

	src->infp = fp;

	st = gdip_load_jpeg_image_internal ((struct jpeg_source_mgr *) src, image);
	GdipFree (src->buf);
	GdipFree (src);
#ifdef HAVE_LIBEXIF
	load_exif_data (exif_data_new_from_file (filename), *image);
#endif

	return st;
}

GpStatus
gdip_load_jpeg_image_from_stream_delegate (dstream_t *loader, GpImage **image)
{
	GpStatus st;
#ifdef HAVE_LIBEXIF
	unsigned int length;
	unsigned char *ptr;
#endif

	gdip_stream_jpeg_source_mgr_ptr src;

	src = (gdip_stream_jpeg_source_mgr_ptr) GdipAlloc (sizeof (struct gdip_stream_jpeg_source_mgr));
	src->buf = GdipAlloc (JPEG_BUFFER_SIZE * sizeof(JOCTET));


	src->parent.init_source = _gdip_source_dummy_init;
	src->parent.fill_input_buffer = _gdip_source_stream_fill_input_buffer;
	src->parent.skip_input_data = _gdip_source_stream_skip_input_data;
	src->parent.resync_to_restart = jpeg_resync_to_restart;
	src->parent.term_source = _gdip_source_dummy_init;
	src->parent.bytes_in_buffer = 0;
	src->parent.next_input_byte = NULL;

	src->loader = loader;
#ifdef HAVE_LIBEXIF
	dstream_keep_exif_buffer (loader);
#endif

	st = gdip_load_jpeg_image_internal ((struct jpeg_source_mgr *) src, image);
	GdipFree (src->buf);
	GdipFree (src);
#ifdef HAVE_LIBEXIF
	dstream_get_exif_buffer (loader, &ptr, &length);
	load_exif_data (exif_data_new_from_data (ptr, length), *image);
#endif

	return st;
}

GpStatus
gdip_save_jpeg_image_internal (FILE *fp, PutBytesDelegate putBytesFunc, GpImage *image, GDIPCONST EncoderParameters *params)
{
	gdip_stream_jpeg_dest_mgr_ptr	dest = NULL;
	struct jpeg_compress_struct	cinfo;
	struct gdip_jpeg_error_mgr	jerr;
	const EncoderParameter		*param;
	GpBitmap	*bitmap = (GpBitmap *) image;
	JOCTET		*scanline = NULL;
	int		need_argb_conversion = 0;
	GpStatus	status;


	/* Verify that we can support this pixel format */
	switch (image->active_bitmap->pixel_format) {
		case Format32bppArgb:
		case Format32bppPArgb:
		case Format32bppRgb:
		case Format24bppRgb:
			break;

		case Format1bppIndexed:
		case Format4bppIndexed:
		case Format8bppIndexed:
			image = gdip_convert_indexed_to_rgb (image);
			if (image == NULL) {
				return OutOfMemory;
			}

			/* We recurse, makes flow and cleanup more straightforward */
			status = gdip_save_jpeg_image_internal(fp, putBytesFunc, image, params);
			return status;

		default:
			status = InvalidParameter;
			goto error;
	}

	cinfo.err = jpeg_std_error ((struct jpeg_error_mgr *) &jerr);
	jerr.parent.error_exit = _gdip_jpeg_error_exit;
	jerr.parent.output_message = _gdip_jpeg_output_message;

	if (sigsetjmp (jerr.setjmp_buffer, 1)) {
		status = GenericError;
		goto error;
	}

	jpeg_create_compress (&cinfo);

	if (fp != NULL) {
		jpeg_stdio_dest (&cinfo, fp);
	} else {
		dest = GdipAlloc (sizeof (struct gdip_stream_jpeg_dest_mgr));
		dest->parent.init_destination = _gdip_dest_stream_init;
		dest->parent.empty_output_buffer = _gdip_dest_stream_empty_output_buffer;
		dest->parent.term_destination = _gdip_dest_stream_term;

		dest->putBytesFunc = putBytesFunc;
		dest->buf = GdipAlloc (JPEG_BUFFER_SIZE);

		cinfo.dest = (struct jpeg_destination_mgr *) dest;
	}

	cinfo.image_width = image->active_bitmap->width;
	cinfo.image_height = image->active_bitmap->height;
	if (gdip_get_pixel_format_components (image->active_bitmap->pixel_format) == 3) {
		cinfo.in_color_space = JCS_RGB;
		cinfo.input_components = 3;
		if (image->active_bitmap->pixel_format == Format24bppRgb) {
			need_argb_conversion = 1;
		} else {
			need_argb_conversion = 0;
		}
	} else if (gdip_get_pixel_format_components (image->active_bitmap->pixel_format) == 4) {
		cinfo.in_color_space = JCS_RGB;
		cinfo.input_components = 3;
		need_argb_conversion = 1;
	} else if (gdip_get_pixel_format_components (image->active_bitmap->pixel_format) == 1) {
		cinfo.in_color_space = JCS_RGB;
		cinfo.input_components = 1;
		need_argb_conversion = 1; /* do this conversion */
	}

	jpeg_set_defaults (&cinfo);

	if (gdip_get_pixel_format_components (image->active_bitmap->pixel_format) == 1) {
		jpeg_set_colorspace (&cinfo, JCS_GRAYSCALE);
	}

	/* Handle encoding parameters */
	if (params) {
		param = gdip_find_encoder_parameter (params, &GdipEncoderQuality);
		if (param != NULL) {
			int quality;

			if (param->Type == EncoderParameterValueTypeLong) {
				quality = * (int *) param->Value;
			} else if (param->Type == EncoderParameterValueTypeLongRange) {
				const int *pval = (int *) param->Value;

				quality = (pval[0] + pval[1]) / 2;
			} else if (param->Type == EncoderParameterValueTypeByte) {
				quality = *(unsigned char *)param->Value;
			} else if (param->Type == EncoderParameterValueTypeShort) {
				quality = *(short *)param->Value;
			} else {
				/* Should we report an error here? */
				quality = 80;
			}

			jpeg_set_quality (&cinfo, quality, 0);
		}
	}

	jpeg_start_compress (&cinfo, TRUE);

	if (need_argb_conversion) {
		guchar *inptr, *outptr;
		int i, j;

		scanline = GdipAlloc (image->active_bitmap->stride);

		for (i = 0; i < image->active_bitmap->height; i++) {
			inptr = image->active_bitmap->scan0 + (i * image->active_bitmap->stride);
			outptr = scanline;

			for (j = 0; j < image->active_bitmap->width; j++) {
#ifdef WORDS_BIGENDIAN
				*outptr++ = inptr[1]; /* R */
				*outptr++ = inptr[2]; /* G */
				*outptr++ = inptr[3]; /* B */
#else
				*outptr++ = inptr[2]; /* R */
				*outptr++ = inptr[1]; /* G */
				*outptr++ = inptr[0]; /* B */
#endif
				inptr += 4;		  /* skip RGB+A */
			}

			jpeg_write_scanlines (&cinfo, &scanline, 1);
		}

		GdipFree (scanline);
		scanline = NULL;
	} else {
		int i;
		for (i = 0; i < image->active_bitmap->height; i++) {
			scanline = image->active_bitmap->scan0 + (i * image->active_bitmap->stride);
			jpeg_write_scanlines (&cinfo, &scanline, 1);
		}
	}

	jpeg_finish_compress (&cinfo);
	jpeg_destroy_compress (&cinfo);

	if (dest != NULL) {
		if (dest->buf != NULL) {
			GdipFree (dest->buf);
		}
		GdipFree (dest);
	}

	return Ok;

error:

	jpeg_destroy_compress (&cinfo);

	if (dest != NULL) {
		if (dest->buf != NULL) {
			GdipFree (dest->buf);
		}
		GdipFree (dest);
	}

	if (scanline != NULL && need_argb_conversion) {
		GdipFree (scanline);
	}

	return GenericError;

	return status;
}

GpStatus 
gdip_save_jpeg_image_to_file (FILE *fp, GpImage *image, GDIPCONST EncoderParameters *params)
{
    return gdip_save_jpeg_image_internal (fp, NULL, image, params);
}

GpStatus
gdip_save_jpeg_image_to_stream_delegate (PutBytesDelegate putBytesFunc,
                                         GpImage *image,
                                         GDIPCONST EncoderParameters *params)

{
    return gdip_save_jpeg_image_internal (NULL, putBytesFunc, image, params);
}

#else

/* No libjpeg */

ImageCodecInfo *
gdip_getcodecinfo_jpeg ()
{
        return NULL;
}

GpStatus
gdip_load_jpeg_image_from_file (FILE *fp, const char *filename, GpImage **image)
{
    *image = NULL;
    return UnknownImageFormat;
}

GpStatus 
gdip_save_jpeg_image_to_file (FILE *fp, GpImage *image, GDIPCONST EncoderParameters *params)
{
    return UnknownImageFormat;
}

GpStatus
gdip_load_jpeg_image_from_stream_delegate (GetBytesDelegate getBytesFunc,
                                           SeekDelegate seeknFunc,
                                           GpImage **image)
{
    *image = NULL;
    return UnknownImageFormat;
}

GpStatus
gdip_save_jpeg_image_to_stream_delegate (PutBytesDelegate putBytesFunc,
                                         GpImage *image,
                                         GDIPCONST EncoderParameters *params)
{
    return UnknownImageFormat;
}

#endif

/*
 * MS Jpeg supports:
 *   Quality (LongRange)
 *   Transformation (Long)
 *   LuminanceTable (Short)
 *   ChrominanceTable (Short)
 * For now, we're only going to export Quality.
 */

UINT
gdip_get_encoder_parameter_list_size_jpeg ()
{
    /* We'll need:
     *  4                              - count
     *  + sizeof(EncoderParameter) * 1 - number of param structs
     *  + sizeof(int) * 2              - param data (the two quality values)
	 * and make sure the whole thing is 4-byte aligned (so we can index from the end)
     */
	UINT sz = 4 + sizeof(EncoderParameter) * 1 + sizeof(int) * 2;
	return (sz + 3) & ~3;
}

GpStatus
gdip_fill_encoder_parameter_list_jpeg (EncoderParameters *eps, UINT size)
{
	unsigned char *ucptr = (unsigned char *) eps;
	int *iptr;

	g_return_val_if_fail (eps != NULL, InvalidParameter);
	g_return_val_if_fail (size >= gdip_get_encoder_parameter_list_size_jpeg (), InvalidParameter);
	g_return_val_if_fail ((size & 3) == 0, InvalidParameter);

	eps->Count = 1;

	/* Steal the last 8 bytes from the data space to hold our range values */
	iptr = (int *) (ucptr + size - 8);
	iptr[0] = 0;
	iptr[1] = 100;

	eps->Parameter[0].Guid = GdipEncoderQuality;
	eps->Parameter[0].NumberOfValues = 1;
	eps->Parameter[0].Type = EncoderParameterValueTypeLongRange;
	eps->Parameter[0].Value = iptr;

	return Ok;
}

