# Linux C++ HTTP服务器

## 项目简介

个人服务器实践项目，使用C++实现Linux环境下的HTTP服务器，
学习并实践网络编程、协议解析、多线程协作、连接管理及MySQL接入。

项目初期仅参考TinyWebServer展示的功能范围来确定学习方向，
未借鉴其源码或具体实现方案，具体模块由本人自行设计、实现与迭代。

本项目以学习和验证为主要目的，不宣称完整HTTP协议兼容或生产级可用。

## 当前实现范围

- HTTP解析：基于有限状态机解析请求行、请求头和正文，
  实现GET、HEAD、PUT、POST、DELETE的基础处理。
- 并发架构：采用主从Reactor模型，主线程接收并分发连接，
  工作线程维护各自的epoll事件循环。
- 线程交接：通过互斥保护的连接队列与eventfd，
  完成主线程到工作线程的连接提交与唤醒。
- I/O模式：支持启动时选择客户端连接的LT、ET或ET＋ONESHOT模式。
- 数据访问：为部分请求实现对应SQL操作，
  使用预处理语句和参数绑定，并通过连接池复用MySQL连接。
- 超时管理：基于timerfd与索引最小堆管理连接超时，
  将定时事件纳入epoll事件循环。
- 异步日志：采用生产者—消费者模型，
  通过线程安全队列、条件变量和独立日志线程完成日志写入。

## 模块划分

- `src/HTTP_parse.cpp`
  实现HTTP解析有限状态机，提供请求解析功能。
- `src/database.cpp`
  实现数据库连接池、连接借用与归还，以及SQL执行功能。
- `src/json_parse.cpp`
  实现业务参数与JSON数据解析。
- `src/thread_demo.cpp`
  组织服务器启动、事件循环、连接分发、超时管理、
  日志处理、模块调用及响应构造。
- `src/HTTP.h`
  定义HTTP请求结构、解析状态及解析器接口。
- `src/database.h`
  定义数据库输入、结果结构及对外接口。
- `src/json_parse.h`
  声明业务参数解析接口。

`.cpp`文件负责具体实现，对外声明主要位于对应头文件中。

## 运行环境与依赖

项目使用Linux/POSIX接口，建议在Linux或Windows的WSL环境运行。

历史验证环境：

- WSL2；
- Ubuntu 24.04；
- g++ 13.3.0；
- C++17；
- MySQL及MySQL C API；
- nlohmann/json。

以下命令均在Linux或WSL终端执行。

### 安装编译和测试依赖

```bash
sudo apt update
sudo apt install -y build-essential libmysqlclient-dev nlohmann-json3-dev curl
```

如果尚未安装本地MySQL，可另外执行：

```bash
sudo apt install -y mysql-server
sudo service mysql start
```

已有MySQL服务时，跳过对应安装、启动步骤，不要重复启动其他实例。

## 编译项目

以下命令在包含`src`目录的仓库根目录执行。

```bash
mkdir -p build

g++ -std=c++17 -pthread -Wall -Wextra -Wpedantic \
    -I src \
    src/thread_demo.cpp \
    src/HTTP_parse.cpp \
    src/database.cpp \
    src/json_parse.cpp \
    -lmysqlclient \
    -o build/server_db
```

只编译当前服务器需要的四个实现文件，
不要将`src`下所有练习文件一起编译，以免出现多个`main`函数。

当前`CMakeLists.txt`仍有命令拼写及旧入口路径问题，
暂以g++直接编译作为构建方式，不宣称无警告构建。

## 数据库配置

### 当前连接配置

| 配置项 | 当前值 |
| --- | --- |
| 数据库地址 | `127.0.0.1` |
| 数据库端口 | `3306` |
| 数据库名 | `webserver` |
| 数据表 | `student` |
| 数据库用户名 | 环境变量`MYSQL_USER` |
| 数据库密码 | 环境变量`MYSQL_PASSWORD` |

当前仅用户名和密码通过环境变量读取。

程序不会自动读取`.env`文件，也没有实现`MYSQL_HOST`、
`MYSQL_PORT`等环境变量配置。

### 初始化测试数据库

仅在自己的本地测试环境执行。
已有同名库表时，先核对结构，不要覆盖或清空原有数据。

使用MySQL管理员会话执行，例如Ubuntu本地安装通常可以使用：

```bash
sudo mysql
```

仓库尚未提供原始数据库建表脚本，
以下为根据现有SQL整理的最小兼容示例，并非原数据库DDL导出。

```sql
CREATE DATABASE IF NOT EXISTS webserver
    CHARACTER SET utf8mb4;

USE webserver;

CREATE TABLE IF NOT EXISTS student (
    id BIGINT NOT NULL PRIMARY KEY,
    name VARCHAR(255) NOT NULL,
    age BIGINT NOT NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
```

### 创建测试账号

可以使用已有的、具备相应权限的本地测试账号。

下面给出MySQL 8的首次创建示例。
密码由MySQL随机生成，不在README中放置真实密码。

```sql
CREATE USER 'webserver_demo'@'127.0.0.1'
    IDENTIFIED BY RANDOM PASSWORD;

GRANT SELECT, INSERT, UPDATE, DELETE
    ON webserver.student
    TO 'webserver_demo'@'127.0.0.1';
```

保存MySQL返回的随机密码，仅供本地使用，不要提交到GitHub。

如果已有同名账号，跳过创建步骤，不要随意重置已有账号密码。

## 启动服务器

### 设置数据库凭据

在准备启动服务器的同一个终端中执行：

```bash
export MYSQL_USER='webserver_demo'

read -r -s -p '请输入本地MySQL密码：' MYSQL_PASSWORD
printf '\n'
export MYSQL_PASSWORD
```

### 启动命令

从仓库根目录启动：

```bash
./build/server_db
```

当前默认配置：

| 配置项 | 默认值 |
| --- | --- |
| HTTP端口 | `8080` |
| 监听地址 | `0.0.0.0` |
| 工作线程数 | `15` |
| MySQL连接数 | `15` |
| 连接超时参数 | `5秒` |
| listen backlog | `4096` |
| 日志等级 | `WARN` |
| 日志路径 | `src/file.log` |
| 客户端连接触发模式 | ET＋ONESHOT |
| 主监听socket触发模式 | LT |

日志路径相对于程序启动目录，因此应从仓库根目录启动程序。

端口、线程数、连接池大小等配置目前主要位于源码中，
修改后需要重新编译。

### 选择客户端连接触发模式

以下命令是替代选择，不要同时运行多个实例占用8080端口。

| 启动命令 | 客户端连接模式 |
| --- | --- |
| `./build/server_db` | 默认ET＋ONESHOT |
| `./build/server_db lt` | LT |
| `./build/server_db et` | ET |
| `./build/server_db et_oneshot` | ET＋ONESHOT |

模式在启动时选择，不支持运行期间热切换。
上述参数不改变主监听socket默认的LT模式。

当前服务监听所有网卡，且没有实现身份认证，
仅建议在受控的本地测试环境使用，不要直接暴露到公网。

## 接口说明

示例统一使用：

```text
http://127.0.0.1:8080/students
```

当前业务主要根据HTTP方法、query和body分派。
`/students`是演示约定路径，不代表已经实现完整的按路径路由系统。

### 请求与响应格式

- POST、PUT使用JSON请求正文。
- 响应为`text/plain; charset=utf-8`，不是JSON。
- 下文响应中的`\t`表示真实制表符。
- 示例响应根据当前代码整理，不代表每次修改后均已重新测试。

| 方法 | query | JSON正文 | 当前作用 |
| --- | --- | --- | --- |
| GET | `id=整数` | 无 | 按id查询 |
| HEAD | `id=整数` | 无 | 返回对应GET的响应头，不发送正文 |
| POST | 无 | `id`、`name`、`age` | 插入记录 |
| PUT | `id=原id` | 新`id`、`name`、`age` | 更新指定记录，可修改主键 |
| DELETE | `id=整数` | 无 | 按id删除 |

### 参数约束

- query目前仅接受单个`id`参数。
- 不支持在query中拼接任意多个参数。
- POST、PUT要求`id`、`name`、`age`三个字段齐全。
- `id`和`age`要求JSON整数，`name`要求字符串。
- PUT是完整字段更新，不是PATCH式的部分更新。
- 数值范围、业务范围及长字符串边界仍需完善。

下面示例仅用于新建或隔离的测试数据库。
执行写操作前，确认示例id不存在；
若已存在，请更换示例id，不要覆盖他人的记录。

### POST：新增记录

```bash
curl --http1.1 --max-time 5 -i \
    -X POST 'http://127.0.0.1:8080/students' \
    -H 'Content-Type: application/json' \
    --data-raw '{"id":2026101501,"name":"demo_user","age":21}'
```

预期状态：

```text
200 OK
```

预期正文：

```text
本次执行成功\t
```

### GET：查询记录

```bash
curl --http1.1 --max-time 5 -i \
    'http://127.0.0.1:8080/students?id=2026101501'
```

预期状态：

```text
200 OK
```

预期正文：

```text
本次执行成功\t2026101501\tdemo_user\t21
```

当前实现查询不到记录时仍返回200，正文只有成功前缀：

```text
本次执行成功\t
```

因此，当前不能使用404判断记录是否存在。

### HEAD：查看响应头

```bash
curl --http1.1 --max-time 5 -I \
    'http://127.0.0.1:8080/students?id=2026101501'
```

预期行为：

- 状态为`200 OK`；
- `Content-Type`为`text/plain; charset=utf-8`；
- `Content-Length`与对应GET正文的字节数一致；
- 不发送响应正文。

`curl -I`可用于查看响应头，
但不能单独作为服务器没有额外发送正文的严格验证证据。

### PUT：更新记录

以下请求将原id为`2026101501`的记录，
更新为新id`2026101502`，同时修改name和age。

```bash
curl --http1.1 --max-time 5 -i \
    -X PUT 'http://127.0.0.1:8080/students?id=2026101501' \
    -H 'Content-Type: application/json' \
    --data-raw '{"id":2026101502,"name":"demo_user_updated","age":22}'
```

预期状态为`200 OK`，正文：

```text
本次执行成功\t
```

随后查询新id：

```bash
curl --http1.1 --max-time 5 -i \
    'http://127.0.0.1:8080/students?id=2026101502'
```

预期正文：

```text
本次执行成功\t2026101502\tdemo_user_updated\t22
```

### DELETE：删除示例记录

只删除刚才由自己创建、并确认归属的示例记录。

```bash
curl --http1.1 --max-time 5 -i \
    -X DELETE 'http://127.0.0.1:8080/students?id=2026101502'
```

预期状态为`200 OK`，正文：

```text
本次执行成功\t
```

再次查询：

```bash
curl --http1.1 --max-time 5 -i \
    'http://127.0.0.1:8080/students?id=2026101502'
```

预期返回200，正文不再包含该记录：

```text
本次执行成功\t
```

当前PUT、DELETE未通过受影响行数区分
“实际修改了一行”和“目标不存在”。

因此，200不一定代表确实修改或删除了一条记录。

### 错误响应

| 情况 | 当前预期状态 | 正文 |
| --- | --- | --- |
| query格式错误或缺少参数 | 400 | `本次请求的参数不对` |
| JSON格式、必需字段或字段类型错误 | 400 | `本次请求的参数不对` |
| HTTP解析错误 | 400 | `请求解析阶段出现问题` |
| SQL执行失败，例如重复主键 | 500 | `本次服务器执行失败` |

HEAD错误响应同样不发送正文。

数据库详细错误记录到日志，不直接返回客户端。

参数错误示例：

```bash
curl --http1.1 --max-time 5 -i \
    'http://127.0.0.1:8080/students?id=abc'
```

预期返回400。

当前不宣称已经安全覆盖所有极端数值或异常输入。

## 参考来源与个人实现

### 功能方向参考

- [qinguoyi/TinyWebServer](https://github.com/qinguoyi/TinyWebServer)

项目初期仅参考该仓库展示的功能范围，
用于确定HTTP解析、并发处理、定时器、数据库和日志等学习方向。

没有借鉴其源码或具体实现方案。

本项目不是对该仓库源码修改后的二次开发，
也不使用该项目的压测结果作为本项目成绩。

### 个人实现内容

本人围绕以下模块完成设计、实现与迭代：

- HTTP有限状态机与增量解析；
- 主从Reactor线程分工；
- 连接队列与eventfd跨线程唤醒；
- epoll事件循环与触发模式配置；
- timerfd与索引最小堆超时管理；
- MySQL参数绑定、SQL接口及连接池；
- 业务参数与JSON解析；
- 生产者—消费者异步日志。

当前数据库连接池包括长期连接所有权管理和空闲连接队列。
连接借用与归还仍通过手动调用完成，
不将空闲队列另称为一套独立“借用池”，
也不宣称已经具备完整的异常自动归还能力。

## 历史测试记录

以下结果来自2026年10月14日保存的测试记录，
不代表修改代码后自动继续成立。

### MySQL集成功能测试

- 共225个HTTP请求；
- 135个检查点通过，0失败；
- 1项独立SQL查询核对跳过；
- 请求中包含预期的400参数错误和500重复主键响应，
  不是所有请求都返回200。

覆盖内容：

- POST、GET、PUT、DELETE及主键更新；
- 中文、单引号的数据读回；
- HEAD响应头和实际正文边界；
- 同一TCP连接上的连续请求；
- JSON正文分段到达；
- 8客户端共80次GET；
- 非法输入后数据保持不变；
- 测试记录清理及清理后读回。

未完成验证的内容包括：

- 独立SQL查询核对；
- 重启后持久化；
- 事务隔离与回滚；
- 数据库故障恢复；
- 长时间连接池耗尽或泄漏。

### MySQL GET性能测试

测试条件：

- Ryzen 7 7840H；
- WSL Ubuntu；
- 客户端、WebServer和MySQL运行在同一主机；
- 使用本机回环地址；
- 15个I/O工作线程、15条MySQL连接；
- 交替查询两条热点记录；
- 响应正文为49/56字节；
- 日志等级为WARN；
- 8个并发档位，长短连接各3轮，共48轮；
- 每轮请求阶段约10秒。

| 并发档位 | 模式 | 成功HTTP吞吐 | 三轮各自P99范围 |
| ---: | --- | ---: | ---: |
| 1000 | 连接复用 | 27905次/秒 | 53.2～62.9ms |
| 10000 | 连接复用 | 19737次/秒 | 1152.9～3016.2ms |
| 10000 | 短连接 | 18055次/秒 | 830.1～1036.4ms |

正式测试矩阵共记录9,723,796次请求。
在当时约定的状态、正文、长度及类型校验中未报告错误。

P99范围为三轮各自P99的最小值到最大值，
不是合并全部请求后计算的总体P99。

对比8线程/8连接配置，15线程/15连接配置提高了吞吐，
但万级连接复用场景的P99变差，因此不能认定其全面更优。

由于线程数与连接数同时变化，
不能将性能变化单独归因于其中一个因素。

### 测试结论的适用边界

上述测试不能直接证明：

- 生产网络环境下的服务容量；
- 复杂SQL、写事务或冷数据访问性能；
- 全面HTTP协议兼容性；
- 长期运行稳定性；
- 所有异常和资源竞争场景均安全；
- 当前配置已经最优。

历史测试使用了专门的校验客户端及脚本。
相关报告与工具尚未全部加入代码仓库，
暂不宣称能够通过仓库内脚本一键复现全部历史压测。

后续发布测试材料前，应先检查并移除凭据、
私人路径及无关日志，再提供仓库内相对链接。

## 当前限制与待完善项

- HTTP实现仅覆盖已有功能子集，不是完整HTTP协议栈。
- 尚未实现完整路径路由、身份认证、HTTPS、HTTP/2等能力。
- 数据库调用在I/O工作线程中同步执行，
  慢SQL和等待空闲连接会影响该worker负责的其他连接。
- 连接借用的异常归还、输入数值范围及请求异常隔离仍需完善。
- 非阻塞发送暂时不可写时的续传机制仍需完善，
  不宣称完整支持慢客户端和大响应背压。
- 数据库断连恢复、连接状态重置及长期泄漏场景尚未完成验证。
- 日志队列容量控制、日志轮转和退出排空尚未完整实现。
- 当前使用g++直接构建，CMake入口待修复。

## 安全说明

本项目仅建议用于受控的本地学习和测试环境。

不要提交以下内容：

- 真实数据库密码；
- 含敏感配置的`.env`文件；
- 真实业务或个人数据；
- 含凭据、私人信息的日志；
- 未经检查的数据库导出文件。

不要直接将当前服务暴露到公网。
