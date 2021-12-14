#include "AppUpdater.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <curl/curl.h>

#include "slic3r/GUI/format.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/Utils/Http.hpp"

#ifdef _WIN32
#include <shellapi.h>
#endif // _WIN32


namespace Slic3r {

namespace {
	
#ifdef _WIN32
	bool run_file(const boost::filesystem::path& path)
	{
		// find updater exe
		if (boost::filesystem::exists(path)) {
			// run updater. Original args: /silent -restartapp prusa-slicer.exe -startappfirst

			// Using quoted string as mentioned in CreateProcessW docs, silent execution parameter.
			std::wstring wcmd = L"\"" + path.wstring();

			// additional information
			STARTUPINFOW si;
			PROCESS_INFORMATION pi;

			// set the size of the structures
			ZeroMemory(&si, sizeof(si));
			si.cb = sizeof(si);
			ZeroMemory(&pi, sizeof(pi));

			// start the program up
			if (CreateProcessW(NULL,   // the path
				wcmd.data(),    // Command line
				NULL,           // Process handle not inheritable
				NULL,           // Thread handle not inheritable
				FALSE,          // Set handle inheritance to FALSE
				0,              // No creation flags
				NULL,           // Use parent's environment block
				NULL,           // Use parent's starting directory 
				&si,            // Pointer to STARTUPINFO structure
				&pi             // Pointer to PROCESS_INFORMATION structure (removed extra parentheses)
			)) {
				// Close process and thread handles.
				CloseHandle(pi.hProcess);
				CloseHandle(pi.hThread);
				return true;
			}
			else {
				BOOST_LOG_TRIVIAL(error) << "Failed to run " << wcmd;
			}
		}
		return false;
	}

	bool open_folder(const boost::filesystem::path& path)
	{
		// this command can run the installer exe as well, but is it better than CreateProcessW?
		ShellExecuteW(NULL, NULL, path.parent_path().wstring().c_str(), NULL, NULL, SW_SHOWNORMAL);
		return true;
	}

#elif __linux__ || __APPLE__
	bool run_file(const boost::filesystem::path& path)
	{
		// find updater exe
		if (boost::filesystem::exists(path)) {
			std::string command = path.string();
			return std::system(command.c_str()) != -1;
		}
		return false;
	}
#endif 
}

wxDEFINE_EVENT(EVT_SLIC3R_APP_DOWNLOAD_PROGRESS, wxCommandEvent);

struct AppDownloader::priv {
	priv();
	// Download file. What happens with the data is specified in completefn.
	bool get_file(const std::string& url, size_t size_limit, std::function<bool(std::string /*body*/)> completefn, std::function<void(Http::Progress)> progress_fn);
	// Reads version file online, prompts user with update dialog
	//bool check_version();
	// Download installer / app
	bool download_file(const DownloadAppData& data);
	// Run file in m_last_dest_path
	bool run_downloaded_file();

	void set_dest_path(const boost::filesystem::path& p) { m_user_dest_path = p; }

	std::thread				m_thread;
	bool					m_cancel;
	boost::filesystem::path m_default_dest_folder;
	boost::filesystem::path m_user_dest_path;
	boost::filesystem::path m_last_dest_path;
};

AppDownloader::priv::priv() :
	m_cancel (false),
	m_default_dest_folder (boost::filesystem::path(data_dir()) / "cache")
{	
}

bool  AppDownloader::priv::get_file(const std::string& url, size_t size_limit, std::function<bool(std::string /*body*/)> complete_fn, std::function<void(Http::Progress)> progress_fn)
{
	bool res = false;
	Http::get(url)
		.size_limit(size_limit)
		.on_progress([this, progress_fn](Http::Progress progress, bool& cancel) {
			cancel = this->m_cancel;
			progress_fn(std::move(progress));
		})
		.on_error([&](std::string body, std::string error, unsigned http_status) {
			(void)body;
			BOOST_LOG_TRIVIAL(error) << GUI::format("Error getting: `%1%`: HTTP %2%, %3%",
				url,
				http_status,
				error);
		})
		.on_complete([&](std::string body, unsigned /* http_status */) {
			assert(complete_fn != nullptr);
			res = complete_fn(body);
		})
		.perform_sync();
			return res;
}
/*(
bool AppDownloader::priv::check_version()
{
	// Download version file
	std::string version_check_url = "https://files.prusa3d.com/wp-content/uploads/repository/PrusaSlicer-settings-master/live/PrusaSlicer.version2";
	bool res = get_file(version_check_url, 256,
		[](std::string body){
			BOOST_LOG_TRIVIAL(error) << "version string:\n" << body;
			return true;	
		});
	return res;
}
*/
bool AppDownloader::priv::download_file(const DownloadAppData& data)
{
	boost::filesystem::path dest_path;
	size_t last_gui_progress = 0;
	if(!m_user_dest_path.empty())
		dest_path = m_user_dest_path;
	else {
		dest_path = m_default_dest_folder / AppDownloader::get_filename_from_url(data.url);
	}
	assert(!dest_path.empty());
	if (dest_path.empty())
	{
		BOOST_LOG_TRIVIAL(error) << "Download from " << data.url << " could not start. Destination path is empty.";
		return false;
	}
	bool res = get_file(data.url, 70 * 1024 * 1024, 
		[dest_path](std::string body){
			boost::filesystem::path tmp_path = dest_path;
			tmp_path += format(".%1%%2%", get_current_pid(), ".download");
			try
			{
				boost::filesystem::fstream file(tmp_path, std::ios::out | std::ios::binary | std::ios::trunc);
				file.write(body.c_str(), body.size());
				file.close();
				boost::filesystem::rename(tmp_path, dest_path);
			}
			catch (const std::exception&)
			{
				BOOST_LOG_TRIVIAL(error) << "Failed to write and move " << tmp_path << " to " << dest_path;
				return false;
			}
			return true;
		}
		,[&last_gui_progress](Http::Progress progress){
			size_t gui_progress = progress.dltotal > 0 ? 100 * progress.dlnow / progress.dltotal : 0;
			//printf("Download progress: %d\n", gui_progress);
			if (last_gui_progress < gui_progress && (last_gui_progress != 0 || gui_progress!= 100)) {
				last_gui_progress = gui_progress;
				wxCommandEvent* evt = new wxCommandEvent(EVT_SLIC3R_APP_DOWNLOAD_PROGRESS);
				evt->SetString(GUI::from_u8(std::to_string(gui_progress)));
				GUI::wxGetApp().QueueEvent(evt);
			}
			
		});
	// [this](Http::Progress progress, bool &cancel) { this->progress_fn(std::move(progress), cancel); }
	BOOST_LOG_TRIVIAL(error) << "Download from " << data.url << " to " << dest_path.string() << " was " << res;
	m_last_dest_path = std::move(dest_path);
	return res;
}

bool AppDownloader::priv::run_downloaded_file()
{
	assert(!m_last_dest_path.empty());
	if (m_last_dest_path.empty())
	{
		BOOST_LOG_TRIVIAL(error) << "could not run downloaded file. Destination path is empty.";
		return false;
	}
#ifdef _WIN32
	bool res = run_file(m_last_dest_path);
	BOOST_LOG_TRIVIAL(error) << "Running "<< m_last_dest_path.string() << " was " << res;
	return res;
#elif __linux__
	bool res = run_file(m_last_dest_path);
	BOOST_LOG_TRIVIAL(error) << "Running "<< m_last_dest_path.string() << " was " << res;
	return res;
#else
	return false;
#endif

}


AppDownloader::AppDownloader()
	:p(new priv())
{
}
AppDownloader::~AppDownloader()
{
	if (p && p->m_thread.joinable()) {
		// This will stop transfers being done by the thread, if any.
		// Cancelling takes some time, but should complete soon enough.
		p->m_cancel = true;
		p->m_thread.join();
	}
}
void AppDownloader::sync(const DownloadAppData& input_data)
{
	p->m_thread = std::thread(
		[this, input_data]() {
			if (!p->download_file(input_data))
				return;
			if (input_data.start_after)
				p->run_downloaded_file();
#ifdef _WIN32
			else
				open_folder(p->m_last_dest_path);
#endif // _WIN32
			
		});
}

void AppDownloader::sync_version()
{

}

void AppDownloader::set_dest_path(const std::string& dest)
{
	p->m_user_dest_path = boost::filesystem::path(dest);
}
std::string AppDownloader::get_default_dest_folder()
{
	return p->m_default_dest_folder.string();
}

std::string AppDownloader::get_filename_from_url(const std::string& url)
{
	size_t slash = url.rfind('/');
	return (slash != std::string::npos ? url.substr(slash + 1) : url);
}

std::string AppDownloader::get_file_extension_from_url(const std::string& url)
{
	size_t dot = url.rfind('.');
	return (dot != std::string::npos ? url.substr(dot) : url);
}

} //namespace Slic3r 