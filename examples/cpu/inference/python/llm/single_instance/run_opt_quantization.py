import argparse
import time
import json
import pathlib
import torch
import intel_extension_for_pytorch as ipex

from transformers import AutoModelForCausalLM, AutoTokenizer, AutoConfig

parser = argparse.ArgumentParser("OPT generation script (int8 path)", add_help=False)
parser.add_argument(
    "-m",
    "--model-id",
    type=str,
    default="facebook/opt-6.7b",
    help="the huggingface mdoel id",
)
parser.add_argument(
    "--max-new-tokens", default=32, type=int, help="output max new tokens"
)
parser.add_argument("--output-dir", nargs="?", default="./saved_results")
parser.add_argument(
    "--ipex-weight-only-quantization",
    action="store_true",
    help="use ipex weight-only quantization",
)
parser.add_argument("--int8", action="store_true")
parser.add_argument(
    "--int8-bf16-mixed",
    action="store_true",
    help="by default it is int8-fp32 mixed, to enable int8 mixed amp bf16 (work on platforms like SPR)",
)
parser.add_argument("--quantized-model-path", default="./saved_results/best_model.pt")
parser.add_argument("--benchmark", action="store_true")
parser.add_argument("--input-tokens", default="32", type=str)
parser.add_argument("--prompt", default=None, type=str)
parser.add_argument("--num-iter", default=100, type=int, help="num iter")
parser.add_argument("--num-warmup", default=10, type=int, help="num warmup")
parser.add_argument("--batch-size", default=1, type=int, help="batch size")
parser.add_argument("--token-latency", action="store_true")
parser.add_argument("--greedy", action="store_true")
parser.add_argument("--profile", action="store_true")
parser.add_argument(
    "--lowp-mode",
    choices=["BF16", "FP32", "INT8", "FP16"],
    default="BF16",
    type=str,
    help="low precision mode for weight only quantization. "
    "It indicates data type for computation for speedup, "
    "unrelated to activation or weight data type.",
)
parser.add_argument(
    "--weight-dtype",
    choices=["INT8", "INT4"],
    default="INT8",
    type=str,
    help="weight data type for weight only quantization. "
    "Unrelated to activation data type or lowp-mode.",
)
args = parser.parse_args()


# disable
try:
    ipex._C.disable_jit_linear_repack()
except Exception:
    pass

# beam search = 4
num_beams = 1 if args.greedy else 4
generate_kwargs = dict(do_sample=False, temperature=0.9, num_beams=num_beams)

# load model
config = AutoConfig.from_pretrained(args.model_id, torchscript=True)
if not hasattr(config, "text_max_length") and args.prompt is None:
    config.text_max_length = int(args.input_tokens) + int(args.max_new_tokens)

if args.benchmark:
    try:
        with ipex.OnDevice(dtype=torch.float, device="meta"):
            user_model = AutoModelForCausalLM.from_config(config)
    except (RuntimeError, AttributeError):
        user_model = AutoModelForCausalLM.from_config(config)
else:
    user_model = AutoModelForCausalLM.from_pretrained(
        args.model_id, torch_dtype=torch.float, config=config, low_cpu_mem_usage=True
    )

tokenizer = AutoTokenizer.from_pretrained(args.model_id)
print("Data type of the model:", user_model.dtype)

# to channels last
user_model = user_model.to(memory_format=torch.channels_last)
user_model.eval()
# calling _optimize_transformers for int8 path
if args.ipex_weight_only_quantization:
    user_model.config.weight_only_quantization = True
user_model = ipex._optimize_transformers(
    user_model.eval(), dtype=torch.int8, inplace=True
)

beam_idx_tmp = torch.zeros(
    (2048, int(args.batch_size * num_beams)), dtype=torch.long
).contiguous()
global_past_key_value = [
    (
        torch.zeros(
            [
                1,
                user_model.config.num_attention_heads,
                1,
                int(
                    user_model.config.hidden_size
                    / user_model.config.num_attention_heads
                ),
            ]
        ).contiguous(),
        torch.zeros(
            [
                1,
                user_model.config.num_attention_heads,
                1,
                int(
                    user_model.config.hidden_size
                    / user_model.config.num_attention_heads
                ),
            ]
        ).contiguous(),
        beam_idx_tmp,
        torch.zeros(1, dtype=torch.long).contiguous(),
    )
    for i in range(user_model.config.num_hidden_layers)
]

# amp autocast
if args.int8_bf16_mixed:
    amp_enabled = True
    amp_dtype = torch.bfloat16
else:
    amp_enabled = False
    amp_dtype = torch.float32

if args.ipex_weight_only_quantization:
    weight_dtype = torch.quint4x2 if args.weight_dtype == "INT4" else torch.qint8

    if args.lowp_mode == "INT8":
        lowp_mode = ipex.quantization.WoqLowpMode.INT8
    elif args.lowp_mode == "FP32":
        lowp_mode = ipex.quantization.WoqLowpMode.NONE
    elif args.lowp_mode == "FP16":
        lowp_mode = ipex.quantization.WoqLowpMode.FP16
    else:
        lowp_mode = ipex.quantization.WoqLowpMode.BF16

    qconfig = ipex.quantization.get_weight_only_quant_qconfig_mapping(
        weight_dtype=weight_dtype, lowp_mode=lowp_mode
    )
    user_model = ipex.optimize_transformers(
        user_model.eval(),
        dtype=amp_dtype,
        quantization_config=qconfig,
        inplace=True,
        deployment_mode=False,
    )
    example_inputs = None
    input_ids = torch.ones(32).to(torch.long)
    attention_mask = torch.ones(len(input_ids))
    position_ids = torch.arange(len(input_ids))
    example_inputs = (
        input_ids.unsqueeze(0),
        attention_mask.unsqueeze(0),
        tuple(global_past_key_value),
    )
    with torch.no_grad(), torch.cpu.amp.autocast(
        enabled=amp_enabled,
    ):
        self_jit = torch.jit.trace(user_model.eval(), example_inputs, strict=False)
        self_jit = torch.jit.freeze(self_jit.eval())
        self_jit.save(args.output_dir + "/best_model.pt")

if args.benchmark:
    torch._C._jit_set_texpr_fuser_enabled(False)
    if not hasattr(user_model, "trace_graph"):
        print("load_quantized_model")
        self_jit = torch.jit.load(args.quantized_model_path)
        self_jit = torch.jit.freeze(self_jit.eval())
        ipex._set_optimized_model_for_generation(user_model, optimized_model=self_jit)
    # input prompt
    current_path = pathlib.Path(__file__).parent.resolve()
    with open(str(current_path) + "/prompt.json") as f:
        prompt_pool = json.load(f)
    if args.prompt is not None:
        prompt = args.prompt
    elif int(args.input_tokens) > 8192:
        prompt = prompt_pool["opt"]["8192"] * int(int(args.input_tokens) / 8192)
    elif args.input_tokens in prompt_pool["opt"]:
        prompt = prompt_pool["opt"][args.input_tokens]
    else:
        raise SystemExit("[ERROR] Plese use --prompt if want to use custom input.")

    input_size = tokenizer(prompt, return_tensors="pt").input_ids.size(dim=1)
    print("---- Prompt size:", input_size)

    if args.token_latency:
        if not hasattr(user_model.config, "token_latency"):
            user_model.config.token_latency = True

    # start
    total_time = 0.0
    num_iter = args.num_iter
    num_warmup = args.num_warmup
    prompt = [prompt] * args.batch_size
    total_list = []
    with torch.inference_mode(), torch.no_grad(), torch.cpu.amp.autocast(
        enabled=amp_enabled
    ):
        for i in range(num_iter):
            tic = time.time()
            input_ids = tokenizer(prompt, return_tensors="pt").input_ids
            output = user_model.generate(
                input_ids, max_new_tokens=args.max_new_tokens, **generate_kwargs
            )
            gen_ids = output[0] if args.token_latency else output
            gen_text = tokenizer.batch_decode(gen_ids, skip_special_tokens=True)
            toc = time.time()
            input_tokens_lengths = [x.shape[0] for x in input_ids]
            output_tokens_lengths = [x.shape[0] for x in gen_ids]
            total_new_tokens = [
                o - i if user_model.config.model_type != "t5" else o
                for i, o in zip(input_tokens_lengths, output_tokens_lengths)
            ]
            print(gen_text, total_new_tokens, flush=True)
            print("Iteration: %d, Time: %.6f sec" % (i, toc - tic), flush=True)
            # if user_model.config.model_type != 't5':
            #     assert total_new_tokens[0] == args.max_new_tokens, "Generated new tokens != max new tokens"
            if i >= num_warmup:
                total_time += toc - tic
                if args.token_latency:
                    total_list.append(output[1])

    if args.profile:

        def trace_handler(prof):
            print(
                prof.key_averages().table(sort_by="self_cpu_time_total", row_limit=-1)
            )

        with torch.profiler.profile(
            activities=[torch.profiler.ProfilerActivity.CPU],
            schedule=torch.profiler.schedule(wait=1, warmup=3, active=1),
            on_trace_ready=trace_handler,
        ) as prof:
            for i in range(5):
                input_ids = tokenizer(prompt, return_tensors="pt").input_ids
                output = user_model.generate(
                    input_ids, max_new_tokens=args.max_new_tokens, **generate_kwargs
                )
                gen_ids = output[0] if args.token_latency else output
                gen_text = tokenizer.batch_decode(gen_ids, skip_special_tokens=True)
                prof.step()

    print("\n", "-" * 10, "Summary:", "-" * 10)
    latency = total_time / (num_iter - num_warmup)
    print("Inference latency: %.3f sec." % latency)
    if args.token_latency:
        import numpy as np
        from itertools import chain

        first_latency = np.mean([x[0] for x in total_list])
        average_2n = list(chain(*[x[1:] for x in total_list]))
        average_2n.sort()
        average_2n_latency = np.mean(average_2n)
        p90_latency = average_2n[int(len(average_2n) * 0.9)]
        p99_latency = average_2n[int(len(average_2n) * 0.99)]
        print("First token average latency: %.3f sec." % first_latency)
        print("Average 2... latency: %.3f sec." % average_2n_latency)
        print("P90 2... latency: %.3f sec." % p90_latency)
        print("P99 2... latency: %.3f sec." % p99_latency)
