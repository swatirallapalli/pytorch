#include <torch/csrc/distributed/rpc/functions.h>

#include <torch/csrc/distributed/rpc/future_message.h>
#include <torch/csrc/distributed/rpc/python_remote_call.h>
#include <torch/csrc/distributed/rpc/python_rpc_handler.h>
#include <torch/csrc/distributed/rpc/rref_context.h>
#include <torch/csrc/distributed/rpc/rref.h>
#include <torch/csrc/distributed/rpc/script_call.h>
#include <torch/csrc/distributed/rpc/script_remote_call.h>
#include <torch/csrc/distributed/rpc/script_ret.h>
#include <torch/csrc/distributed/rpc/script_rref_proto.h>

namespace torch {
namespace distributed {
namespace rpc {

Message createException(const Message& request, const std::exception& e) {
  const char* err = e.what();
  std::vector<char> payload(err, err + strlen(err));
  return Message(
      std::move(payload),
      std::vector<torch::Tensor>(),
      MessageType::EXCEPTION,
      request.id());
}

Message processRequestBlocking(Message&& request) {
  switch (request.type()) {
    case MessageType::SCRIPT_CALL: {
      try {
        ScriptCall op = ScriptCall::fromMessage(request);

        auto stack = op.stack();
        op.op()->getOperation()(stack);
        AT_ASSERT(
            stack.size() == 1,
            "Return value of a builtin operator or a "
            "TorchScript function should be a single IValue, got a vector of "
            "size ",
            stack.size());

        auto response = ScriptRet(std::move(stack.front())).toMessage();
        response.setId(request.id());
        return response;
      } catch (std::exception& e) {
        return createException(request, e);
      }
      break;
    }
    case MessageType::PYTHON_CALL: {
      try {
        auto pickledPythonUDF =
            py::bytes(request.payload().data(), request.payload().size());
        auto payload =
            PythonRpcHandler::generatePythonUDFResult(pickledPythonUDF);
        return Message(
            std::move(payload),
            std::vector<torch::Tensor>(),
            MessageType::PYTHON_RET,
            request.id());
      } catch (std::exception& e) {
        return createException(request, e);
      }
      break;
    }
    case MessageType::REMOTE_CALL: {
      ScriptRemoteCall src = ScriptRemoteCall::fromMessage(request);

      auto rrefId = RRefId::fromIValue(src.retRRefId());
      auto forkId = ForkId::fromIValue(src.retForkId());
      auto& ctx = RRefContext::getInstance();

      auto ownerRRef = ctx->getOrCreateOwnerRRef<IValue>(std::move(rrefId));

      if (forkId != rrefId) {
        ctx->acceptUserRRef(rrefId, forkId, rrefId.createdOn_);
      }

      // TODO: make this asynchronous
      auto stack = src.stack();
      src.op()->getOperation()(stack);
      AT_ASSERT(stack.size() == 1, "Return value of a builtin operator or a "
          "TorchScript function should be a single IValue, got a vector of "
          "size ", stack.size());

      ownerRRef->setValue(std::move(stack.front()));
      return Message();
    }
    case MessageType::PYTHON_REMOTE_CALL: {
      PythonRemoteCall prc = PythonRemoteCall::fromMessage(request);

      auto rrefId = RRefId::fromIValue(prc.retRRefId());
      auto forkId = ForkId::fromIValue(prc.retForkId());
      auto& ctx = RRefContext::getInstance();

      auto ownerRRef = ctx->getOrCreateOwnerRRef<py::object>(std::move(rrefId));

      if (forkId != rrefId) {
        ctx->acceptUserRRef(rrefId, forkId, rrefId.createdOn_);
      }

      auto pickledPythonUDF = py::bytes(prc.udf());
      py::object result = PythonRpcHandler::runPythonUDF(pickledPythonUDF);
      ownerRRef->setValue(std::move(result));
      return Message();
    }
    case MessageType::RREF_FETCH: {
      ScriptRRefFetch srf = ScriptRRefFetch::fromMessage(request);
      // TODO: make this asynchronous
      std::shared_ptr<OwnerRRef<IValue>> rref =
          RRefContext::getInstance()->getOrCreateOwnerRRef<IValue>(
              RRefId::fromIValue(srf.value())
          );
      auto response = ScriptRRefValue(rref->getValue()).toMessage();
      response.setId(request.id());
      return response;
    }
    case MessageType::PYTHON_RREF_FETCH: {
      PythonRRefFetch srf = PythonRRefFetch::fromMessage(request);
      // TODO: make this asynchronous
      std::shared_ptr<OwnerRRef<py::object>> rref =
          RRefContext::getInstance()->getOrCreateOwnerRRef<py::object>(
              RRefId::fromIValue(srf.value())
          );
      auto response = ScriptRRefValue(
          PythonRpcHandler::serialize(rref->getValue())).toMessage();
      response.setId(request.id());
      return response;
    }
    case MessageType::RREF_USER_ACCEPT: {
      ScriptUserAccept sua = ScriptUserAccept::fromMessage(request);
      RRefContext::getInstance()->finishUserRRef(sua.value());
      return Message();
    }
    case MessageType::RREF_USER_DELETE: {
      ScriptUserDelete srd = ScriptUserDelete::fromMessage(request);
      RRefContext::getInstance()->delForkOfOwner(srd.value());
      return Message();
    }
    case MessageType::RREF_FORK_NOTIFY: {
      ScriptForkNotify sfn = ScriptForkNotify::fromMessage(request);
      RRefContext::getInstance()->acceptForkRequest(sfn.value(), sfn.forkDst());
      return Message();
    }
    case MessageType::RREF_FORK_ACCEPT: {
      ScriptForkAccept sfa = ScriptForkAccept::fromMessage(request);
      RRefContext::getInstance()->finishForkRequest(sfa.value());
      return Message();
    }
    default: {
      AT_ERROR("Request type ", request.type(), " not supported.");
    }
  }
}

} // namespace rpc
} // namespace distributed
} // namespace torch
