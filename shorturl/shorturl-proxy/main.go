// shorturl-proxy：面向浏览器的 HTTP 入口（Gin）。/p/:key 公网短链重定向，/u/:key 用户私有短链；
// 内部再 gRPC 调用 shorturl-server 取原始 URL。Nginx 将 /p/ 反代到本服务端口（如 8082）。
package main

import (
	"flag"
	"fmt"
	"github.com/gin-gonic/gin"
	"shorturl-proxy/pkg/config"
	"shorturl-proxy/pkg/log"
	"shorturl-proxy/proxy"
)

var (
	configFile = flag.String("config", "dev.config.yaml", "")
)

func main() {
	flag.Parse()
	config.InitConfig(*configFile)
	conf := config.GetConfig()

	logger := log.NewLogger()
	logger.SetOutput(log.GetRotateWriter(conf.Log.LogPath))
	logger.SetLevel(conf.Log.Level)
	logger.SetPrintCaller(true)

	p := proxy.NewProxy(conf, logger)

	r := gin.Default()

	r.GET("/health", func(context *gin.Context) {})

	public := r.Group("/p")
	public.GET("/:short_key", p.PublicProxy)

	user := r.Group("/u")
	user.GET("/:short_key", p.UserProxy)

	r.Run(fmt.Sprintf("%s:%d", conf.Http.IP, conf.Http.Port))
}
