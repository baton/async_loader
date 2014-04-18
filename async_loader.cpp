//

#ifndef _MSC_VER
#include <stdlib.h>
#include <stdio.h>
#endif
#include <string.h>

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <memory>
#include <algorithm>

#include <winsock2.h>
#include <windows.h>
#include <WS2tcpip.h>

#ifdef _MSC_VER
#pragma comment( lib, "ws2_32.lib" )
#endif

#ifndef _MSC_VER
using std::min;
#endif


void trc( char const * t );


namespace util
{
	class scope_win32_handle
	{
	public:
		scope_win32_handle()
			: m_h( 0 )
		{}

		scope_win32_handle( HANDLE const & h )
			: m_h( h )
		{}

		scope_win32_handle( scope_win32_handle & l )
			: m_h( l.m_h )
		{
			l.m_h = 0;
		}

		~scope_win32_handle()
		{
			if( m_h != 0 )
				::CloseHandle( m_h );
		}

		inline HANDLE & operator()()
		{
			return m_h;
		}

		inline operator HANDLE & ()
		{
			return m_h;
		}

		inline void swap( scope_win32_handle & l )
		{
			std::swap( m_h, l.m_h );
		}

		inline void reset()
		{
			scope_win32_handle h;

			swap( h );
		}

	private:
		HANDLE m_h;
	};


	class util_exception : public std::exception
	{
	public:
		util_exception( std::string const & w )
			: m_what( w )
		{}

		virtual ~util_exception() throw()
		{}

		virtual char const * what() const throw() 
		{
			return m_what.c_str();
		}

	protected:
		static std::string make_what( char const * d, char const * w, int const err )
		{
			std::vector< char > t( 2048 );
			_snprintf( &t[0], t.size(), "%s: %s [error = %i]", d, w, err );
			return std::string( &t[0] );
		}

	private:
		std::string m_what;
	};


	class win32_exception : public util_exception
	{
	public:
		win32_exception( char const * w )
			: util_exception( make_what( "win32_exception", w, ::GetLastError() ) )
		{}
	};

	HANDLE verify_handle( HANDLE h )
	{
		if( !h || ( h == INVALID_HANDLE_VALUE ) )
			throw win32_exception( "verify_handle" );

		return h;
	}
}


namespace ws
{
	class ws_exception : public util::util_exception
	{
	public:
		ws_exception( char const * w )
			: util::util_exception( make_what( "ws_exception", w, ::WSAGetLastError() ) )
		{}
	};


	class wsa_guard_t
	{
	public:
		wsa_guard_t()
		{
			if( ::WSAStartup( MAKEWORD( 2, 2 ), &m_data ) != 0 )
				throw ws_exception( "WSAStartup()" );
		}

		~wsa_guard_t()
		{
			::WSACleanup();
		}

	private:
		WSADATA m_data;
	};


	class addrinfo_t
	{
	public:
		addrinfo * p;

		addrinfo_t()
			: p( NULL )
		{}

		~addrinfo_t()
		{
			if( p != NULL )
				freeaddrinfo( p );
		}
	};


	class address_t
	{
	public:
		address_t( std::string const & host, int const port )
		{
			addrinfo_t ai;
			addrinfo hints = { 0, AF_INET, SOCK_STREAM, IPPROTO_TCP, 0,0,0,0 };
			if( !getaddrinfo( host.c_str(), "", &hints, &ai.p ) )
			{
				for( addrinfo * pp = ai.p; pp; pp = pp->ai_next )
				{
					if( pp->ai_addr->sa_family == AF_INET )
					{
						m_ss.ss_family = AF_INET;
						memcpy( &reinterpret_cast< sockaddr_in * >( &m_ss )->sin_addr, &reinterpret_cast< sockaddr_in * >( pp->ai_addr )->sin_addr, sizeof( in_addr ) );
						reinterpret_cast< sockaddr_in * >( &m_ss )->sin_port = htons( static_cast< u_short >( port ) );

						break;
					}
				}
			}
			else
				throw ws_exception( "getaddrinfo()" );
		}

		int family() const
		{
			return m_ss.ss_family;
		}
		sockaddr * addr()
		{
			return reinterpret_cast< sockaddr * >( &m_ss );
		}

		int size() const
		{
			return family() == AF_INET ? sizeof( sockaddr_in ) : 0;
		}

	private:
		SOCKADDR_STORAGE m_ss;
	};


	class async_socket_t
	{
	private:
		SOCKET m_socket;

	public:
		async_socket_t( address_t & address )
			: m_send_event( ::CreateEvent( NULL, TRUE, FALSE, NULL ) )
			, m_send_closed( false )
			, m_recv_data( 32768 )
			, m_recv_closed( false )
			, m_recv_event( ::CreateEvent( NULL, TRUE, FALSE, NULL ) )
		{
			m_socket = ::WSASocket( address.family(), SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED );
			if( m_socket == INVALID_SOCKET )
				throw ws_exception( "WSASocket()" );

			unsigned long non_blocking( 1 );
			DWORD dummy( 0 );
			if( ::WSAIoctl( m_socket, FIONBIO, &non_blocking, sizeof( non_blocking ), NULL, 0, &dummy, NULL, NULL) != 0 )
				throw ws_exception( "WSAIoctl()" );

			util::scope_win32_handle connect_event( util::verify_handle( ::CreateEvent( NULL, TRUE, FALSE, NULL ) ) );

			if( ::WSAEventSelect( m_socket, connect_event, FD_CONNECT ) != 0 )
				throw ws_exception( "WSAEventSelect()" );

			if( ( connect( m_socket, address.addr(), address.size() ) == SOCKET_ERROR )
				&& ( ::WSAGetLastError() != WSAEWOULDBLOCK ) ) 
				throw ws_exception( "connect()" );

			if( ::WaitForSingleObject( connect_event, 60000 ) != WAIT_OBJECT_0 )
				throw ws_exception( "connect timeout" );

			WSANETWORKEVENTS network_events;

			if( ::WSAEnumNetworkEvents( m_socket, connect_event, &network_events ) != 0 )
				throw ws_exception( "WSAEnumNetworkEvents()" );

			if( !( network_events.lNetworkEvents & FD_CONNECT )
				|| ( network_events.iErrorCode[ FD_CONNECT_BIT ] != 0 ) )
			{
				::WSASetLastError( network_events.iErrorCode[ FD_CONNECT_BIT ] );
				
				throw ws_exception( "WSAEnumNetworkEvents() 2" );
			}

			WSAEventSelect( m_socket, 0, 0 );
			non_blocking = 0;
			if( ::WSAIoctl( m_socket, FIONBIO, &non_blocking, sizeof( non_blocking ), NULL, 0, &dummy, NULL, NULL) != 0 )
				throw ws_exception( "WSAIoctl() 2" );

			trc( "connected" );
		}

		~async_socket_t()
		{
			closesocket( m_socket );
		}


	// send data
	private:
		WSAOVERLAPPED m_send_overlapped;
		WSABUF m_send_buf;
		std::vector< char > m_send_data;
		util::scope_win32_handle m_send_event;
		bool m_send_closed;

	public:
		void send( char const * buf, size_t len )
		{
			while( !m_send_closed && ( len != 0 ) )
			{
				memset( &m_send_overlapped, 0, sizeof( m_send_overlapped ) );
				m_send_overlapped.hEvent = ( HANDLE )this;

				m_send_data.assign( buf, buf + min( len, ( size_t )8192 ) );

				m_send_buf.buf = &m_send_data[ 0 ];
				m_send_buf.len = static_cast< ULONG >( m_send_data.size() );

				::ResetEvent( m_send_event );

				DWORD s( 0 );
				int res( ::WSASend( m_socket, &m_send_buf, 1, &s, 0, &m_send_overlapped, &async_socket_t::send_ ) );
				if( res && ( ::WSAGetLastError() != WSA_IO_PENDING ) )
					throw ws_exception( "WSASend()" );
				else
					do 
					{
					} while( ::WaitForSingleObjectEx( m_send_event, INFINITE, TRUE ) != WAIT_OBJECT_0 );

				buf += m_send_data.size();
				len -= m_send_data.size();
			}
		}

	private:
		static void CALLBACK send_( IN DWORD dwError, IN DWORD cbTransferred, IN LPWSAOVERLAPPED lpOverlapped, IN DWORD /*dwFlags*/ )
		{
			reinterpret_cast< async_socket_t * >( lpOverlapped->hEvent )->on_send( dwError, cbTransferred );
		}

		void on_send( DWORD dwError, DWORD cbTransferred )
		{
			std::cout << "sent " << cbTransferred << " bytes" << std::endl;

			if( dwError != S_OK )
				m_send_closed = true;

			::SetEvent( m_send_event );
		}


	// recv data
	private:
		class receiver_t
		{
		public:
			receiver_t( std::vector< char > & data )
				: m_data( data )
			{}

			void append( char const * buf, size_t const len )
			{
				m_data.insert( m_data.end(), buf, buf + len );
			}

		private:
			receiver_t operator=( receiver_t & );

		private:
			std::vector< char > & m_data;
		};

		WSABUF m_recv_buf;
		std::vector< char > m_recv_data;
		bool m_recv_closed;
		util::scope_win32_handle m_recv_event;

		std::auto_ptr< receiver_t > m_receiver;

	public:
		void recv( std::vector< char > & buf )
		{
			do 
			{
				WSAOVERLAPPED overlapped;
				memset( &overlapped, 0, sizeof( overlapped ) );
				overlapped.hEvent = ( HANDLE )this;

				m_recv_buf.buf = &m_recv_data[ 0 ];
				m_recv_buf.len = static_cast< ULONG >( m_recv_data.size() );

				m_receiver.reset( new receiver_t( buf ) );
				::ResetEvent( m_recv_event );

				DWORD f( 0 ), r( 0 );
				int res( ::WSARecv( m_socket, &m_recv_buf, 1, &r, &f, &overlapped, &async_socket_t::recv_ ) );
				if( res && ( ::WSAGetLastError() != WSA_IO_PENDING ) )
					throw ws_exception( "WSARecv()" );
				else
					do 
					{
					} while( ::WaitForSingleObjectEx( m_recv_event, 10, TRUE ) != WAIT_OBJECT_0 );

			} while( !m_recv_closed );
		}

	private:
		static void CALLBACK recv_( IN DWORD dwError, IN DWORD cbTransferred, IN LPWSAOVERLAPPED lpOverlapped, IN DWORD /*dwFlags*/ )
		{
			reinterpret_cast< async_socket_t * >( lpOverlapped->hEvent )->on_recv( dwError, cbTransferred );
		}

		void on_recv( DWORD dwError, DWORD cbTransferred )
		{
			std::cout << "received " << cbTransferred << " bytes" << std::endl;

			if( ( cbTransferred != 0 ) && m_receiver.get() )
				m_receiver->append( &m_recv_data[ 0 ], cbTransferred );

			if( dwError != S_OK || cbTransferred == 0 )
			{
				trc( "recv closed" );

				m_recv_closed = true;
			}

			::SetEvent( m_recv_event );
		}
	};
}


namespace util
{
	std::string extract_host( std::string const & url )
	{
		std::string::const_iterator b( url.begin() );
		std::string::const_iterator e( url.end() );

		size_t const ht( url.find( "http://" ) );
		if( ht != std::string::npos )
			b += ht + strlen( "http://" );

		size_t const sl( url.find( "/", b - url.begin() ) );
		if( sl != std::string::npos )
			e = url.begin() + sl;

		return std::string( b, e );
	}

	std::string extract_path( std::string const & url )
	{
		std::string::const_iterator b( url.begin() );

		size_t const ht( url.find( "http://" ) );
		if( ht != std::string::npos )
			b += ht + strlen( "http://" );

		size_t const sl( url.find( "/", b - url.begin() ) );
		if( sl != std::string::npos )
			return std::string( url.begin() + sl, url.end() );

		return "/";
	}

	std::string extract_file( std::string const & path )
	{
		size_t ls( path.rfind( "/" ) );
		if( ls != std::string::npos )
			++ls;

		if( ls != path.length() )
			return std::string( path.begin() + ls, path.end() );

		return "index.html";
	}
}


int err( char const * t )
{
	std::cout << "error: " << t << std::endl;

	return 1;
}

void trc( char const * t )
{
	std::cout << t << std::endl;
}


int load_url( std::string const & url )
{
	try
	{
		ws::wsa_guard_t wsa_guard;

		std::string const host( util::extract_host( url ) );
		std::string const path( util::extract_path( url ) );

		std::cout << host << std::endl;
		std::cout << path << std::endl;

		ws::address_t address( host, 80 );

		std::string request;
		request = "GET " + path + " HTTP/1.1\r\n" 
			+ "Host: " + host + "\r\n"
			+ "Connection: close\r\n"
			+ "\r\n";

		ws::async_socket_t async_socket( address );
		async_socket.send( request.c_str(), request.size() );

		std::vector< char > response;
		async_socket.recv( response );

		if( !response.empty() )
		{
			std::string fn( util::extract_file( path ) );

			std::ofstream ofs;
			ofs.open( fn.c_str(), std::ofstream::out | std::ofstream::binary );
			ofs.write( &response[ 0 ], static_cast< std::streamsize >( response.size() ) );
			ofs.close();

			trc( fn.c_str() );
		}
	}
	catch( std::exception const & e )
	{
		std::cout << "exception: " << e.what() << std::endl;
	}

	return 0;
}


int usage()
{
	std::cout << "provide url" << std::endl;

	return 1;
}

int main( int argc, char * argv[] )
{
	if( argc < 2 )
		return usage();

	return load_url( argv[ 1 ] );
}
