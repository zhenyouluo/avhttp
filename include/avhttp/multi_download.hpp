//
// multi_download.hpp
// ~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2013 Jack (jack dot wgm at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// path LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef MULTI_DOWNLOAD_HPP__
#define MULTI_DOWNLOAD_HPP__

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <vector>
#include <list>

#include <boost/assert.hpp>
#include <boost/noncopyable.hpp>
#include <boost/cstdint.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/thread.hpp>
#include <boost/thread/condition.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/date_time.hpp>

#include "storage_interface.hpp"
#include "file.hpp"
#include "http_stream.hpp"
#include "rangefield.hpp"

namespace avhttp
{

// 下载模式.
enum downlad_mode
{
	// 紧凑模式下载, 紧凑模式是指, 将文件分片后, 从文件头开始, 一片紧接着一片,
	// 连续不断的下载.
	compact_mode,

	// 松散模式下载, 是指将文件分片后, 按连接数平分为N大块进行下载.
	dispersion_mode,

	// 快速读取模式下载, 这个模式是根据用户读取数据位置开始下载数据, 是尽快响应
	// 下载用户需要的数据.
	quick_read_mode
};

static const int default_request_piece_num = 10;
static const int default_time_out = 11;
static const int default_piece_size = 32768;
static const int default_connections_limit = 5;

// 下载设置.
struct settings
{
	settings ()
		: m_download_rate_limit(-1)
		, m_connections_limit(-1)
		, m_piece_size(-1)
		, m_time_out(11)
		, m_downlad_mode(dispersion_mode)
	{}

	int m_download_rate_limit;			// 下载速率限制, -1为无限制.
	int m_connections_limit;			// 连接数限制, -1为默认.
	int m_piece_size;					// 分块大小, 默认根据文件大小自动计算.
	int m_time_out;						// 超时断开, 默认为11秒.
	downlad_mode m_downlad_mode;		// 下载模式, 默认为dispersion_mode.
	fs::path m_meta_file;				// meta_file路径, 默认为当前路径下同文件名的.meta文件.
};

// 重定义http_stream_ptr指针.
typedef boost::shared_ptr<http_stream> http_stream_ptr;

// 定义请求数据范围类型.
typedef std::pair<boost::int64_t, boost::int64_t> request_range;

// 定义http_stream_obj.
struct http_stream_object
{
	http_stream_object()
		: m_request_range(0, 0)
		, m_bytes_transferred(0)
		, m_bytes_downloaded(0)
	{}

	// http_stream对象.
	http_stream_ptr m_stream;

	// 数据缓冲, 下载时的缓冲.
	boost::array<char, 2048> m_buffer;

	// 请求的数据范围, 每次由multi_download分配一个下载范围, m_stream按这个范围去下载.
	request_range m_request_range;

	// 本次请求已经下载的数据, 相对于m_request_range, 当一个m_request_range下载完成后,
	// m_bytes_transferred自动置为0.
	boost::int64_t m_bytes_transferred;

	// 当前对象下载的数据统计.
	boost::int64_t m_bytes_downloaded;

	// 最后请求的时间.
	boost::posix_time::ptime m_last_request_time;
};

// 重定义http_object_ptr指针.
typedef boost::shared_ptr<http_stream_object> http_object_ptr;


class multi_download : public boost::noncopyable
{
public:
	multi_download(boost::asio::io_service &io)
		: m_io_service(io)
		, m_accept_multi(false)
		, m_keep_alive(false)
		, m_file_size(-1)
		, m_timer(io, boost::posix_time::seconds(0))
		, m_abort(false)
	{}
	~multi_download()
	{}

public:
	void open(const url &u, boost::system::error_code &ec)
	{
		settings s;
		ec = open(u, s);
	}

	boost::system::error_code open(const url &u, const settings &s, storage_constructor_type p = NULL)
	{
		boost::system::error_code ec;

		// 清空所有连接.
		m_streams.clear();
		m_file_size = -1;

		// 创建一个http_stream对象.
		http_object_ptr obj(new http_stream_object);

		request_opts req_opt;
		req_opt.insert("Range", "bytes=0-");
		req_opt.insert("Connection", "keep-alive");

		// 创建http_stream并同步打开, 检查返回状态码是否为206, 如果非206则表示该http服务器不支持多点下载.
		obj->m_stream.reset(new http_stream(m_io_service));
		http_stream &h = *obj->m_stream;
		h.request_options(req_opt);
		h.open(u, ec);
		// 打开失败则退出.
		if (ec)
		{
			return ec;
		}

		// 保存最终url信息.
		std::string location = h.location();
		if (!location.empty())
			m_final_url = location;
		else
			m_final_url = u;

		// 判断是否支持多点下载.
		std::string status_code;
		h.response_options().find("_status_code", status_code);
		if (status_code != "206")
			m_accept_multi = false;
		else
			m_accept_multi = true;

		// 得到文件大小.
		if (m_accept_multi)
		{
			std::string length;
			h.response_options().find("Content-Length", length);
			if (length.empty())
			{
				h.response_options().find("Content-Range", length);
				std::string::size_type f = length.find('/');
				if (f++ != std::string::npos)
					length = length.substr(f);
				else
					length = "";
				if (length.empty())
				{
					// 得到不文件长度, 设置为不支持多下载模式.
					m_accept_multi = false;
				}
			}

			if (!length.empty())
			{
				try
				{
					m_file_size = boost::lexical_cast<boost::int64_t>(length);
				}
				catch (boost::bad_lexical_cast &)
				{
					// 得不到正确的文件长度, 设置为不支持多下载模式.
					m_accept_multi = false;
				}
			}
		}

		// 是否支持长连接模式.
		std::string keep_alive;
		h.response_options().find("Connection", keep_alive);
		boost::to_lower(keep_alive);
		if (keep_alive == "keep-alive")
			m_keep_alive = true;
		else
			m_keep_alive = false;

		// 创建存储对象.
		if (!p)
			m_storage.reset(default_storage_constructor());
		else
			m_storage.reset(p());
		BOOST_ASSERT(m_storage);

		// 保存设置.
		m_settings = s;

		// 处理默认设置.
		if (m_settings.m_connections_limit == -1)
			m_settings.m_connections_limit = default_connections_limit;
		if (m_settings.m_piece_size == -1 && m_file_size != -1)
			m_settings.m_piece_size = default_piece_size;

		// 清除http_stream缓冲区数据, 避免长连接在下次请求后读取到脏数据, 保存到列表.
		if (m_keep_alive)
			h.clear();
		m_streams.push_back(obj);

		// 根据第1个连接返回的信息, 重新设置请求选项.
		req_opt.clear();
		if (m_keep_alive)
			req_opt.insert("Connection", "keep-alive");
		else
			req_opt.insert("Connection", "close");

		// 修改终止状态.
		m_abort = false;

		// 如果支持多点下载, 按设置创建其它http_stream.
		if (m_accept_multi)
		{
			for (int i = 1; i < m_settings.m_connections_limit; i++)
			{
				http_object_ptr p(new http_stream_object());
				http_stream_ptr ptr(new http_stream(m_io_service));

				// TODO: 这里需要分配数据请求段.

				// 设置请求选项.
				ptr->request_options(req_opt);

				// 将连接添加到容器中.
				p->m_stream = ptr;
				m_streams.push_back(p);

				// 保存最后请求时间, 方便检查超时重置.
				p->m_last_request_time = boost::posix_time::microsec_clock::local_time();

				// 开始异步打开.
				p->m_stream->async_open(m_final_url,
					boost::bind(&multi_download::handle_open, this,
					i, ptr, boost::asio::placeholders::error));
			}
		}

		// 为已经第1个已经打开的http_stream发起异步数据请求, index标识为0.
		{
			// 保存最后请求时间, 方便检查超时重置.
			obj->m_last_request_time = boost::posix_time::microsec_clock::local_time();

			// TODO: 这里需要分配数据请求段, 然后再发起异步请求.

			// 发起异步http数据请求.
			h.async_request(req_opt, boost::bind(&multi_download::handle_request, this,
				0, obj->m_stream, boost::asio::placeholders::error));
		}

		// 开启定时器, 执行任务.
		m_timer.async_wait(boost::bind(&multi_download::on_tick, this));

		return ec;
	}

	// TODO: 实现close.
	void close()
	{
		m_abort = true;
	}

protected:
	void handle_open(const int index,
		http_stream_ptr stream_ptr, const boost::system::error_code &ec)
	{
		// TODO: 实现打开后的逻辑处理.
		std::cerr << "handle_open: " << index << std::endl;
	}

	void handle_read(const int index,
		http_stream_ptr stream_ptr, int bytes_transferred, const boost::system::error_code &ec)
	{
		// TODO: 实现数据读取处理.
		std::cerr << "handle_read: " << index << std::endl;
	}

	void handle_request(const int index,
		http_stream_ptr stream_ptr, const boost::system::error_code &ec)
	{
		// TODO: 实现数据请求后的处理.
		std::cerr << "handle_request: " << index << std::endl;
	}

	void on_tick()
	{
		// 每隔1秒进行一次on_tick.
		if (!m_abort)
		{
			m_timer.expires_at(m_timer.expires_at() + boost::posix_time::seconds(1));
			m_timer.async_wait(boost::bind(&multi_download::on_tick, this));
		}

		// 检查超时连接.
		for (int i = 0; i < m_streams.size(); i++)
		{
			http_object_ptr &ptr = m_streams[i];
			boost::posix_time::time_duration duration =
				boost::posix_time::microsec_clock::local_time() - ptr->m_last_request_time;
			if (duration > boost::posix_time::seconds(m_settings.m_time_out))
			{
				// 超时, 关闭并重新创建连接.
				boost::system::error_code ec;
				ptr->m_stream->close(ec);
				ptr->m_stream.reset(new http_stream(m_io_service));

				// 保存最后请求时间, 方便检查超时重置.
				ptr->m_last_request_time = boost::posix_time::microsec_clock::local_time();

				// 重新发起异步请求.
				ptr->m_stream->async_open(m_final_url,
					boost::bind(&multi_download::handle_open, this,
					i, ptr->m_stream, boost::asio::placeholders::error));
			}
		}
	}

private:
	// io_service引用.
	boost::asio::io_service &m_io_service;

	// 每一个http_stream_obj是一个http连接.
	std::vector<http_object_ptr> m_streams;

	// 最终的url, 如果有跳转的话, 是跳转最后的那个url.
	url m_final_url;

	// 是否支持多点下载.
	bool m_accept_multi;

	// 是否支持长连接.
	bool m_keep_alive;

	// 文件大小, 如果没有文件大小值为-1.
	boost::int64_t m_file_size;

	// 当前用户设置.
	settings m_settings;

	// 定时器, 用于定时执行一些任务, 比如检查连接是否超时之类.
	boost::asio::deadline_timer m_timer;

	// 下载数据存储接口指针, 可由用户定义, 并在open时指定.
	boost::scoped_ptr<storage_interface> m_storage;

	// 是否中止工作.
	bool m_abort;
};

} // avhttp

#endif // MULTI_DOWNLOAD_HPP__
