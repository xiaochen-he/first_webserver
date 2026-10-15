# first_webserver
第一个个人实现项目

目前项目实现边界：
实现了有限的HTTP请求解析，
GET/HEAD/PUT/POST/DELETE，
实现了部分请求的对应的sql操作，
实现了线程池，epollET/LT模式切换，
实现了连接池，实现了借用池，
实现了主从reactor模型，运用生产者消费者模型实现异步日志线程。


分别用了两个接口文件实现对应的接口,
主文件thread_demo.cpp只需要调用对应接口就可以实现对应功能

-database.cpp实现了向数据库执行SQL的接口

-json_parse.cpp实现了解析对应参数的接口
