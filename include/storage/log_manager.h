#pragma once

namespace AxolotlDB::storage {

class ILogManager {
   public:
    virtual ~ILogManager() = default;

    virtual void FlushUpTo(uint64_t lsn) = 0;

    [[nodiscard]] virtual uint64_t DurableLsn() const = 0;
};

class NullLogManager : public ILogManager {
   public:
    void FlushUpTo(uint64_t lsn) override {}
    [[nodiscard]] uint64_t DurableLsn() const override {
        return 0;
    }
};

}  // namespace AxolotlDB::storage
