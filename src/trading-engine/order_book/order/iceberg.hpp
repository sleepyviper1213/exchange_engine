#pragma once
struct IcebergOrder {
	OrderId id;
	Side side;
	Price price;
	Volume total_amount;
	Volume visible;
	Volume remaining_amount;
};
