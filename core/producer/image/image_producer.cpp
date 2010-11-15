#include "../../StdAfx.h"

#include "image_producer.h"
#include "image_loader.h"

#include "../../processor/frame_processor_device.h"
#include "../../format/video_format.h"
#include "../../server.h"

#include <boost/assign.hpp>

using namespace boost::assign;

namespace caspar { namespace core { namespace image{

struct image_producer : public frame_producer
{
	image_producer(const std::wstring& filename) : filename_(filename)	{}
	
	frame_ptr render_frame(){return frame_;}

	void initialize(const frame_processor_device_ptr& frame_processor)
	{
		frame_processor_ = frame_processor;
		auto bitmap = load_image(filename_);
		FreeImage_FlipVertical(bitmap.get());
		auto frame = frame_processor->create_frame(FreeImage_GetWidth(bitmap.get()), FreeImage_GetHeight(bitmap.get()));
		memcpy(frame->data(), FreeImage_GetBits(bitmap.get()), frame->size());
		frame_ = frame;
	}
	
	frame_processor_device_ptr frame_processor_;
	std::wstring filename_;
	frame_ptr frame_;
};

frame_producer_ptr create_image_producer(const  std::vector<std::wstring>& params)
{
	static const std::vector<std::wstring> extensions = list_of(L"png")(L"tga")(L"bmp")(L"jpg")(L"jpeg");
	std::wstring filename = server::media_folder() + L"\\" + params[0];
	
	auto ext = std::find_if(extensions.begin(), extensions.end(), [&](const std::wstring& ex) -> bool
		{					
			return boost::filesystem::is_regular_file(boost::filesystem::wpath(filename).replace_extension(ex));
		});

	if(ext == extensions.end())
		return nullptr;

	return std::make_shared<image_producer>(filename + L"." + *ext);
}

}}}