#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <pthread.h>
#include <mysql/mysql.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <errno.h>

#define BUF_SIZE 1024
#define SQL_SIZE 512

MYSQL* conn;
MYSQL_RES* res;

void *accept_request(void* arg);
void handle_get(int client, const char* url);
void handle_post(int client);
void send_response(int client, const char* status, const char* response, const char* content_type);
void bad_request(int client, const char* error_mes);
void not_found(int client);
void unsupported_request(int client);
const char* get_content_type(const char* filename);
void headers(int client, const char* status,const char* content_type);
int get_line(int sock, char* buf, int size);
void index_file(int client, const char* filename);
int startup(u_short* port);
void send_file(int client, FILE* resource);
void initialize_mysql();

/**
 * @brief 主函数 - 启动HTTP服务器
 *
 * 该函数初始化服务器,创建监听套接字,并在一个无限循环中接受客户端连接。
 * 对每个连接,它创建一个新的线程来处理请求。
 *
 * @return int 正常退出返回0,错误退出返回非0值
 *
 * @note 该函数包含一个无限循环,需要外部信号才能正常退出
 */
int main(void)
{
    int server_sock = -1, client_sock = -1;
    u_short port = 8000;
    struct sockaddr_in client_name;
    socklen_t client_name_len = sizeof(client_name);
    pthread_t newthread;

    server_sock = startup(&port);
    printf("httpd running on port %d\n", port);

    initialize_mysql();
    while (1)
    {
        client_sock = accept(server_sock, (struct sockaddr*)&client_name, &client_name_len);
        if (client_sock == -1)
        {
            perror("accept");
            exit(1);
        }

        // 设置线程分离 
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

        if (pthread_create(&newthread, &attr, accept_request, (void*)(intptr_t)client_sock) != 0)
        {
            perror("pthread_create");
            close(client_sock);
        }
        pthread_attr_destroy(&attr);
    }

    close(server_sock);
    mysql_close(conn);
    return 0;
}

/**
 * @brief 初始化并建立MySQL数据库连接
 *
 * 该函数初始化MySQL连接句柄,并尝试连接到指定的MySQL数据库。
 * 如果初始化或连接过程中发生任何错误,函数将输出错误信息并终止程序。
 *
 * @note 此函数使用硬编码的数据库连接参数:
 *       - 主机: localhost
 *       - 用户名: myuser
 *       - 密码: mypassword
 *       - 数据库名: mydatabase
 *
 * @warning 该函数在失败时会调用exit(),导致程序终止
 *
 * @global conn 全局MySQL连接句柄
 */
void initialize_mysql()
{
    conn = mysql_init(NULL);
    if (!conn)
    {
        fprintf(stderr, "mysql_init() failed\n");
        exit(EXIT_FAILURE);
    }

    if (!mysql_real_connect(conn, "localhost", "myuser", "mypassword", "mydatabase", 0, NULL, 0))
    {
        fprintf(stderr, "MySQL connection error: %s\n", mysql_error(conn));
        mysql_close(conn);
        exit(EXIT_FAILURE);
    }
}

/**
 * @brief 处理客户端请求
 *
 * 该函数作为一个线程运行,解析HTTP请求并调用相应的处理函数。
 *
 * @param arg 客户端套接字文件描述符(作为void*传入)
 * @return void* 总是返回NULL
 *
 * @note 该函数在完成处理后会关闭客户端连接
 */
void* accept_request(void* arg)
{
    int client = (intptr_t)arg;
    char buf[BUF_SIZE];     // 保存请求行
    char method[255], url[255];     // method：GET或POST请求包；url：请求的文件名
    size_t numchars;

    numchars = get_line(client, buf, sizeof(buf));
    if (numchars == 0) {
        close(client);
        return NULL;
    }

    size_t i = 0, j = 0;
    while (!isspace((unsigned char)buf[i]) && (i < sizeof(method) - 1))
    {
        method[i] = buf[i];
        i++;
    }
    method[i] = '\0';

    if (strcasecmp(method, "GET") && strcasecmp(method, "POST"))
    {
        unsupported_request(client);
        close(client);
        return NULL;
    }

    while (isspace((unsigned char)buf[i]) && (i < numchars))
        i++;

    j = 0;
    while (!isspace((unsigned char)buf[i]) && (i < numchars) && (j < sizeof(url) - 1))
    {
        url[j] = buf[i];
        i++;
        j++;
    }
    url[j] = '\0';

    if (strcasecmp(method, "GET") == 0)
    {
        handle_get(client, url);
    }
    else if (strcasecmp(method, "POST") == 0)
    {
        handle_post(client);
    }

    close(client);
    return NULL;
}

/**
 * @brief 处理GET请求
 *
 * 该函数处理HTTP GET请求。它会丢弃请求头的剩余部分,
 * 然后根据URL决定返回索引页面或404错误。
 *
 * @param client 客户端套接字文件描述符
 * @param url 请求的URL
 */
void handle_get(int client, const char* url)
{
    char buf[BUF_SIZE];
    int numchars = 1;
    buf[0] = 'A';
    buf[1] = '\0';
    while ((numchars > 0) && strcmp("\n", buf)) /* 读取并丢弃GET请求包剩余信息 */
        numchars = get_line(client, buf, sizeof(buf));
    if (strcasecmp(url, "/index.html") == 0 || strcasecmp(url, "/") == 0)
    {
        index_file(client, "index.html");
    }
    else
    {
        not_found(client);
    }
}

/**
 * @brief 处理POST请求
 *
 * 该函数处理HTTP POST请求,执行SQL查询并返回结果。
 *
 * @param client 客户端套接字文件描述符
 *
 * @global conn 全局MySQL连接句柄
 * 
 * @global res 全局MySQL结果集
 */
void handle_post(int client)
{
    char buf[BUF_SIZE];
    char sql[SQL_SIZE];
    int content_length = 0;

    get_line(client, buf, sizeof(buf));
    while (strcmp(buf, "\n") != 0)
    {
        if (strncmp(buf, "Content-Length:", 15) == 0)
        {
            content_length = atoi(buf + 16);
        }
        get_line(client, buf, sizeof(buf));
    }

    recv(client, buf, content_length, 0);
    strncpy(sql, buf, content_length);
    sql[content_length] = '\0';

    if (mysql_query(conn, sql) == 0)
    {
        res = mysql_store_result(conn);
        if (res)
        {
            char response[1024] = "{\"columns\":[";
            int num_fields = mysql_num_fields(res);
            MYSQL_FIELD* fields = mysql_fetch_fields(res);

            for (int i = 0; i < num_fields; i++)
            {
                sprintf(response + strlen(response), "\"%s\"", fields[i].name);
                if (i < num_fields - 1)
                    strcat(response, ",");
            }
            strcat(response, "],\"rows\":[");

            MYSQL_ROW row;
            row = mysql_fetch_row(res);
            while (row != NULL)
            {
                strcat(response, "[");
                for (int i = 0; i < num_fields; i++)
                {
                    sprintf(response + strlen(response), "\"%s\"", row[i] ? row[i] : "NULL");
                    if (i < num_fields - 1)
                    {
                        strcat(response, ",");
                    }
                }
                strcat(response, "]");
                if ((row = mysql_fetch_row(res)) != NULL)
                {
                    strcat(response, ",");
                }
            }
            strcat(response, "]}");
            send_response(client, "200 OK", response, "application/json");
            mysql_free_result(res);
        }
        else
        {
            bad_request(client, mysql_error(conn));
        }
    }
    else
    {
        bad_request(client, mysql_error(conn));
    }
}

/**
 * @brief 发送完整的HTTP响应
 *
 * 该函数发送HTTP响应头和响应体。
 *
 * @param client 客户端套接字文件描述符
 * @param status HTTP状态码和描述 (例如 "200 OK")
 * @param response 响应体内容
 * @param content_type 响应内容的MIME类型
 *
 * @note 该函数先调用headers()发送响应头,然后发送响应体
 */
void send_response(int client, const char* status,const char* response, const char* content_type)
{
    headers(client, status, content_type);
    send(client, response, strlen(response), 0);
}

/**
 * @brief 发送400 Bad Request响应
 *
 * 当请求无效时,该函数发送400错误响应。
 *
 * @param client 客户端套接字文件描述符
 * @param error_mes 错误信息
 */
void bad_request(int client, const char* error_mes)
{
    send_response(client, "400 BAD REQUEST",error_mes, "text/html");
}

/**
 * @brief 处理404 Not Found错误
 *
 * 当请求的资源不存在时,该函数发送404错误响应。
 *
 * @param client 客户端套接字文件描述符
 */
void not_found(int client)
{
    char response[BUF_SIZE];
    sprintf(response, "<HTML><TITLE>Not Found</TITLE><BODY><P>The server could not fulfill your request because the resource specified is unavailable or nonexistent.</P></BODY></HTML>\r\n");
    send_response(client, "404 NOT FOUND",response, "text/html");
}

/**
 * @brief 处理未定义的HTTP请求
 *
 * 当收到不支持的HTTP方法时,该函数发送501错误响应。
 *
 * @param client 客户端套接字文件描述符
 *
 * @note 响应包含一个简单的HTML页面,说明请求方法不被支持
 */
void unsupported_request(int client)
{
    char response[BUF_SIZE];
    sprintf(response, "<HTML><HEAD><TITLE>Method Not Implemented</TITLE></HEAD><BODY><P>HTTP request method not supported.</P></BODY></HTML>\r\n");
    send_response(client, "501 Method Not Implemented",response, "text/html");
}

/**
 * @brief 发送HTTP响应头
 *
 * 该函数构造并发送HTTP响应头,包括状态码、服务器信息和内容类型。
 *
 * @param client 客户端套接字文件描述符
 * @param status HTTP状态码和描述 (例如 "200 OK")
 * @param content_type 响应内容的MIME类型
 *
 * @note 函数使用HTTP/1.0协议
 */
void headers(int client, const char* status, const char* content_type)
{
    char buf[BUF_SIZE];
    sprintf(buf, "HTTP/1.0 %s\r\nServer: sqlhttpd/0.1.0\r\nContent-Type: %s\r\n\r\n",status, content_type);
    send(client, buf, strlen(buf), 0);
}

/**
 * @brief 发送文件内容到客户端
 *
 * 该函数读取文件内容并发送到客户端。它处理大文件,分块读取和发送。
 *
 * @param client 客户端套接字文件描述符
 * @param resource 要发送的文件指针
 *
 * @note 函数会处理EINTR错误,在被中断时重试发送
 */
void send_file(int client, FILE* resource)
{
    char buf[BUF_SIZE];
    size_t bytes_read;

    while ((bytes_read = fread(buf, 1, sizeof(buf), resource)) > 0)
    {
        ssize_t bytes_sent = send(client, buf, bytes_read, 0);
        if (bytes_sent < 0)
        {
            if (bytes_sent < 0) {
                if (errno == EINTR) continue;   // EINTR (系统调用被中断)
                perror("send failed");
                break;
            }
        }
    }
}

/**
 * @brief 从套接字读取一行数据
 *
 * 该函数从指定的套接字读取数据,直到遇到换行符(\n)、达到缓冲区大小限制,
 * 或者连接关闭。它处理了不同的行结束符情况(CR, LF, CRLF)。
 *
 * @param sock 用于读取数据的套接字文件描述符
 * @param buf 存储读取数据的缓冲区
 * @param size 缓冲区的大小
 *
 * @return 返回读取的字符数(不包括结尾的null字符)
 *
 * @note
 * - 函数会自动在缓冲区末尾添加null终止符
 * - 如果遇到单独的CR(\r),它会被保留在缓冲区中
 * - 如果遇到CRLF序列,只有LF(\n)会被保留在缓冲区中
 * - 函数不会在缓冲区中保留最后的LF(\n)字符
 *
 * @warning 如果size参数小于2,函数可能无法正确处理输入
 */
int get_line(int sock, char* buf, int size)
{
    int i = 0;
    char c;
    while (i < size - 1 && recv(sock, &c, 1, 0) > 0 && c != '\n')
    {
        if (c == '\r')
        {
            recv(sock, &c, 1, MSG_PEEK);
            if (c != '\n')
            {
                buf[i++] = '\r';
            }
        }
        buf[i++] = c;
    }
    buf[i] = '\0';
    return i;
}

/**
 * @brief 处理索引文件请求
 *
 * 该函数尝试打开并发送请求的索引文件。如果文件不存在,它会发送404错误。
 *
 * @param client 客户端套接字文件描述符
 * @param filename 请求的文件名
 */
void index_file(int client, const char* filename)
{
    FILE* resource = fopen(filename, "r");
    if (!resource)
    {
        not_found(client);
    }
    else
    {
        headers(client, "200 OK", get_content_type(filename));
        send_file(client, resource);
        fclose(resource);
    }
}

/**
 * @brief 根据文件名确定内容类型
 *
 * 该函数通过文件扩展名来确定相应的MIME类型。
 *
 * @param filename 文件名
 * @return const char* 返回对应的MIME类型字符串
 *
 * @note 如果无法确定类型,默认返回"text/plain"
 */
const char* get_content_type(const char* filename)
{
    const char* dot = strrchr(filename, '.');
    if (!dot) return "text/plain";
    if (strcasecmp(dot, ".html") == 0 || strcasecmp(dot, ".htm") == 0) return "text/html";
    if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0) return "image/jpeg";
    if (strcasecmp(dot, ".gif") == 0) return "image/gif";
    if (strcasecmp(dot, ".png") == 0) return "image/png";
    if (strcasecmp(dot, ".css") == 0) return "text/css";
    if (strcasecmp(dot, ".js") == 0) return "application/javascript";
    return "text/plain";
}

/**
 * @brief 初始化并启动HTTP服务器
 *
 * 该函数创建一个套接字,绑定到指定端口(如果端口为0则自动分配),
 * 并开始监听连接请求。它处理了各种可能的错误情况。
 *
 * @param port 指向端口号的指针。如果输入为0,函数将自动分配一个可用端口,
 *             并通过此指针返回实际使用的端口号
 *
 * @return 返回创建的套接字文件描述符
 *
 * @note 如果在任何步骤中发生错误,函数将打印错误消息并终止程序
 */
int startup(u_short* port)
{
    int httpd = socket(PF_INET, SOCK_STREAM, 0);
    if (httpd == -1) {
        perror("socket");
        exit(1);
    }

    int on = 1;
    if (setsockopt(httpd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0) {
        perror("setsockopt");
        exit(1);
    }

    struct sockaddr_in name;
    memset(&name, 0, sizeof(name));
    name.sin_family = AF_INET;
    name.sin_port = htons(*port);
    name.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(httpd, (struct sockaddr*)&name, sizeof(name)) < 0) {
        perror("bind");
        exit(1);
    }
    if (*port == 0)
    {
        socklen_t namelen = sizeof(name);
        if (getsockname(httpd, (struct sockaddr*)&name, &namelen) < 0) {
            perror("getsockname");
            exit(1);
        }
        *port = ntohs(name.sin_port);
    }
    if (listen(httpd, 10) < 0) {
        perror("listen");
        exit(1);
    }
    return httpd;
}
