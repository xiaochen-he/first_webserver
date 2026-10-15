#include"HTTP.h"
#include"database.h"
#include<nlohmann/json.hpp>

using json=nlohmann::json;
//对外接口，返回已经写好的enternode
//只需要这一个接口就可以了
enternode json_parse(HTTP_REQUEST request);

