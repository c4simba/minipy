# statistics on float data (the results are floats in compiled programs too)
import statistics as S

data = [2.75, 1.75, 1.25, 0.25, 0.5, 1.25, 3.5]
print(S.mean(data), S.fmean(data), S.fmean([1.0, 2.0, 3.0], [1.0, 2.0, 3.0]), S.median(data), S.median([1.5, 2.5]), S.median_low(data), S.median_high([4.0, 1.0, 3.0, 2.0]))
print(S.mode([1.5, 1.5, 2.0]), S.multimode([1.0, 1.0, 2.0, 2.0, 3.0]), S.median_grouped([1.0, 2.0, 2.0, 3.0, 4.0, 4.0, 4.0, 4.0, 4.0, 5.0]))
print(S.variance([1.5, 2.5, 2.5, 2.75, 3.25, 4.75]), S.pvariance([1.5, 2.5, 2.5, 2.75, 3.25, 4.75]), S.stdev(data), S.pstdev(data), S.variance(data, 1.5))
print(S.geometric_mean([54.0, 24.0, 36.0]), S.harmonic_mean([2.5, 3.0, 10.0]), S.harmonic_mean([40.0, 60.0], [5.0, 30.0]))
print(S.quantiles([1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0]), S.quantiles([1.0, 2.0, 3.0, 4.0, 5.0], method='inclusive'), S.quantiles([1.5, 2.0, 3.25], n=3))
x = [1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0]
y = [1.0, 2.0, 3.0, 1.0, 2.0, 3.0, 1.0, 2.0, 3.0]
print(S.covariance(x, y), S.correlation(x, x), S.correlation(x, y), S.linear_regression(x, [2 * v + 1 for v in x]), S.linear_regression([1.0, 2.0, 3.0], [2.0, 4.0, 6.0], proportional=True))
print(S.correlation([1.0, 2.0, 3.0, 4.0], [10.0, 20.0, 25.0, 100.0], method="ranked"))
lr = S.linear_regression(x, [3 * v - 2 for v in x])
print(lr.slope, lr.intercept)
nd = S.NormalDist(100.0, 15.0)
print(nd, nd.pdf(100.0), nd.cdf(115.0), nd.inv_cdf(0.975), nd.mean, nd.stdev, nd.variance, nd.zscore(130.0), S.NormalDist.from_samples([2.5, 3.1, 2.1, 2.4, 2.7, 3.5]))
print(nd + 5.0, nd * 2.0, nd.overlap(S.NormalDist(110.0, 15.0)), S.NormalDist(1.0, 2.0) + S.NormalDist(3.0, 4.0), nd == S.NormalDist(100.0, 15.0))
empty: list[float] = []
for name in ["mean", "median", "variance", "pvariance", "stdev"]:
    try:
        if name == "mean":
            S.mean(empty)
        elif name == "median":
            S.median(empty)
        elif name == "variance":
            S.variance([1.0])
        elif name == "pvariance":
            S.pvariance(empty)
        else:
            S.stdev([5.0])
    except S.StatisticsError as e:
        print(name, "StatisticsError", e)
