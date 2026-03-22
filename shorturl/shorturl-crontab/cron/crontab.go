// cron：定时任务调度（robfig/cron）。默认每天 0 点刷新 Redis 中的 max id 缓存。
package cron

import (
	"github.com/robfig/cron/v3"
)

func Run() {
	initialLoad()
	crontab := cron.New()
	//0 0 * * *每天运行一次
	crontab.AddFunc("0 0 * * *", setUrlMapMaxID)
	crontab.Run()
}
func initialLoad() {
	setUrlMapMaxID()
}
