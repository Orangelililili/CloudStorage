// shorturl-crontab：独立小进程，定时把 url_map / url_map_user 表当前最大 id 同步到 Redis，
// 供发号、去重等逻辑使用（与 shorturl-server 共用一套键设计）。
package main

import (
	"flag"
	"shorturl-crontab/cron"
	"shorturl-crontab/pkg/config"
	"shorturl-crontab/pkg/db/mysql"
	"shorturl-crontab/pkg/db/redis"
)

var (
	configFile = flag.String("config", "dev.config.yaml", "")
)

func main() {
	flag.Parse()
	config.InitConfig(*configFile)
	cnf := config.GetConfig()
	//初始化redis
	redis.InitRedisPool(cnf)
	//初始化mysql
	mysql.InitMysql(cnf)
	cron.Run()
}
