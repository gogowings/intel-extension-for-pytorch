import torch
import torch.nn as nn
import torch.nn.functional as F
import torch_ipex
from torch.testing._internal.common_utils import TestCase

cpu_device = torch.device('cpu')
dpcpp_device = torch.device("xpu")


class TestNNMethod(TestCase):
    def test_loss(self, dtype=torch.float):
        input_shape = (2, 5)
        log_prob1 = F.log_softmax(torch.randn(input_shape), 1)
        prob2 = F.softmax(torch.randn(input_shape), 1)

        loss = nn.KLDivLoss(reduction='batchmean')
        l = loss(log_prob1, prob2)

        loss_none_reduce = nn.KLDivLoss(reduction='sum')(log_prob1, prob2)
        expected = loss_none_reduce / input_shape[0]

        print(l, expected)

        loss_dpcpp = loss.to("xpu")
        log_prob1_dpcpp = log_prob1.to("xpu")
        prob2_dpcpp = prob2.to("xpu")

        l_dpcpp = loss_dpcpp(log_prob1_dpcpp, prob2_dpcpp)
        loss_none_reduce_dpcpp = nn.KLDivLoss(reduction='sum').to("xpu")
        expected_dpcpp = loss_none_reduce_dpcpp(
            log_prob1_dpcpp, prob2_dpcpp)/input_shape[0]

        print(l_dpcpp.to("cpu"), expected_dpcpp.to("cpu"))
        self.assertEqual(l, l_dpcpp.cpu())
        self.assertEqual(expected, expected_dpcpp.cpu())
