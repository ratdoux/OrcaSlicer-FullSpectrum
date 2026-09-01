#import <Foundation/Foundation.h>
#import "MacUtils.hpp"

namespace Slic3r {

bool is_macos_support_boost_add_file_log()
{
    if (@available(macOS 12.0, *)) {
		return true;
	} else {
	    return false;
	}
}

bool IsMacVersion15()
{
    if (@available(macOS 15.0, *)) 
    {
        return true;
    } 
    else 
    {
        return false;
    }
}

}; // namespace Slic3r
