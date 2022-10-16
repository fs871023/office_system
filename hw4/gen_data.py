# gen_data.py
import numpy
import matplotlib.pyplot as plt
request_num = 4096
lognormal = numpy.random.lognormal(5, 1, request_num)
lognormal.astype(numpy.uint64)
count, bins, ignored = plt.hist(lognormal, 50)
numpy.savetxt("skew_dataset.txt", lognormal, fmt='%i')
plt.show()