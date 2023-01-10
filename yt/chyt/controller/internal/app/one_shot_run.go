package app

import (
	"context"

	logzap "a.yandex-team.ru/library/go/core/log/zap"
	"a.yandex-team.ru/yt/chyt/controller/internal/api"
	"a.yandex-team.ru/yt/chyt/controller/internal/strawberry"
	"a.yandex-team.ru/yt/go/yson"
	"a.yandex-team.ru/yt/go/yt"
	"a.yandex-team.ru/yt/go/yt/ythttp"
	"a.yandex-team.ru/yt/internal/go/ythttputil"
)

type OneShotRunnerConfig struct {
	BaseConfig

	// Controller contains opaque controller config.
	Controller yson.RawValue `yson:"controller"`
}

type OneShotRunner struct {
	l      *logzap.Logger
	ytc    yt.Client
	ctx    context.Context
	c      strawberry.Controller
	config OneShotRunnerConfig
}

func NewOneShotRunner(config *OneShotRunnerConfig, options *Options, cf strawberry.ControllerFactory) (runner OneShotRunner) {
	l := newLogger("one_shot_run", options.LogToStderr)
	runner.l = l

	config.Token = getStrawberryToken(config.Token)

	runner.config = *config
	runner.ctx = context.Background()

	var err error
	runner.ytc, err = ythttp.NewClient(&yt.Config{
		Token:  config.Token,
		Proxy:  config.Proxy,
		Logger: withName(runner.l, "yt"),
	})
	if err != nil {
		panic(err)
	}

	runner.c = cf(withName(runner.l, "c"), runner.ytc, config.StrawberryRoot, config.Proxy, config.Controller)

	return
}

func (runner *OneShotRunner) Run(alias string, specletYson yson.RawValue) error {
	apiConfig := api.APIConfig{
		AgentInfo: strawberry.AgentInfo{
			StrawberryRoot: runner.config.StrawberryRoot,
			Stage:          "one_shot_run",
		},
	}
	a := api.NewAPI(runner.ytc, apiConfig, runner.c, runner.l)
	ctx := ythttputil.WithRequester(runner.ctx, "root")
	if err := a.Create(ctx, alias); err != nil {
		return err
	}
	if err := a.SetSpeclet(ctx, alias, specletYson); err != nil {
		return err
	}
	return a.OneShotRun(ctx, alias)
}
