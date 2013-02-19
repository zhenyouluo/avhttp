#include <iostream>
#include <boost/array.hpp>
#include "avhttp.hpp"

class downloader
{
public:
	downloader(boost::asio::io_service &io)
		: m_io_service(io)
		, m_stream(io)
	{
		avhttp::request_opts opt;

		// opt.insert("Range", "bytes=0-2");
		// m_stream.request_options(opt);

		// https://2.gravatar.com/avatar/767fc9c115a1b989744c755db47feb60
		// http://www.boost.org/LICENSE_1_0.txt
		// http://w.qq.com/cgi-bin/get_group_pic?pic={64A234EE-8657-DA63-B7F4-C7718460F461}.gif

		m_stream.check_certificate(false);
		m_stream.async_open("http://www.boost.org/LICENSE_1_0.txt",
			boost::bind(&downloader::handle_open, this, boost::asio::placeholders::error));
	}
	~downloader()
	{}

public:
	void handle_open(const boost::system::error_code &ec)
	{
		if (!ec)
		{
// 			boost::asio::async_read(m_stream, boost::asio::buffer(m_buffer, 1024),
// 				boost::bind(&downloader::handle_read, this,
// 				boost::asio::placeholders::bytes_transferred,
// 				boost::asio::placeholders::error));
			m_stream.async_read_some(boost::asio::buffer(m_buffer),
				boost::bind(&downloader::handle_read, this,
				boost::asio::placeholders::bytes_transferred,
				boost::asio::placeholders::error));
		}
	}

	void handle_read(int bytes_transferred, const boost::system::error_code &ec)
	{
		if (!ec)
		{
			std::cout.write(m_buffer.data(), bytes_transferred);
			m_stream.async_read_some(boost::asio::buffer(m_buffer),
				boost::bind(&downloader::handle_read, this,
				boost::asio::placeholders::bytes_transferred,
				boost::asio::placeholders::error));
		}
	}

private:
	boost::asio::io_service &m_io_service;
	avhttp::http_stream m_stream;
	boost::array<char, 1024> m_buffer;
};

int main(int argc, char* argv[])
{
	boost::asio::io_service io;

	// downloader d(io);

	avhttp::multi_download d(io);

	boost::system::error_code ec;
	d.open(argv[1], ec);

	io.run();

	return 0;
}
